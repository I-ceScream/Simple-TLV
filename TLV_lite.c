#include "TLV_lite.h"
#include "FreeRTOS.h"
#include "log.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"
#include <string.h>

extern UART_HandleTypeDef huart6;

/* --- 内部私有结构 --- */
typedef struct {
  instruction_t instruction;          // 指令数据
  instruction_states_t state;         // 当前状态
  comm_instruction_executor executor; // 执行函数
  bool is_sync;                       // 是否同步
  uint32_t start_time;                // 真正开始执行的时间戳
  uint32_t timeout;                   // 超时时间 (Ticks)
  uint32_t result_code;               // 结果码
} comm_instruction_internal_t;

typedef struct {
  comm_instruction_internal_t instructions[COMM_MAX_INSTRUCTION_NUM];
  uint8_t register_bitmap[COMM_INSTRUCTION_ARRAY_SIZE];

  SemaphoreHandle_t mutex;
  QueueHandle_t execute_queue; // 存储 int32_t 类型的索引
  QueueHandle_t result_queue;  // 存储已完成的指令索引

  comm_new_instruction_callback new_inst_cb;
  comm_error_callback error_cb;
  comm_instruction_done_callback done_cb;
} comm_manager_t;

static comm_manager_t cm;

/* --- 内部工具函数 --- */

static int32_t find_free_slot(void) {
  for (int i = 0; i < COMM_MAX_INSTRUCTION_NUM; i++) {
    if (!(cm.register_bitmap[i >> 3] & (1 << (i & 0x07))))
      return i;
  }
  return -1;
}

int32_t comm_find_instruction_slot_by_info(uint8_t obj, uint8_t action) {
  for (int i = 0; i < COMM_MAX_INSTRUCTION_NUM; i++) {
    if ((cm.register_bitmap[i >> 3] & (1 << (i & 0x07)))) {
      if (cm.instructions[i].instruction.obj == obj &&
          cm.instructions[i].instruction.action == action) {
        return i;
      }
    }
  }
  return -1;
}

/* --- 任务实现 --- */

// 1. 指令派发任务 (按顺序消费队列)
static void comm_manager_task(void *params) {
  LOG_DEBUG("comm_manager_task");
  int32_t idx;
  while (1) {
    if (xQueueReceive(cm.execute_queue, &idx, portMAX_DELAY) == pdTRUE) {
      instruction_t inst_copy;
      comm_instruction_executor executor = NULL;
      bool is_sync;

      xSemaphoreTake(cm.mutex, portMAX_DELAY);
      executor = cm.instructions[idx].executor;
      is_sync = cm.instructions[idx].is_sync;
      inst_copy = cm.instructions[idx].instruction;
      if (cm.new_inst_cb)
        cm.new_inst_cb(&inst_copy);

      // 更新状态：异步指令先标记为执行中，如果后面 executor
      cm.instructions[idx].state =
          is_sync ? COMM_EXECUTING_SYNC : COMM_EXECUTING_ASYNC;
      cm.instructions[idx].start_time = xTaskGetTickCount();
      xSemaphoreGive(cm.mutex);

      if (executor) {
        uint32_t err = executor(&inst_copy);

        if (err != 0) {
          // --- 情况 A: 执行器直接报错 (如你日志中的 MOVE FAILED) ---
          xSemaphoreTake(cm.mutex, portMAX_DELAY);
          cm.instructions[idx].result_code = err;
          xQueueSend(cm.result_queue, &idx,
                     0); // 无论同步异步，报错就直接进结果队列释放 Slot
          xSemaphoreGive(cm.mutex);
        } else if (is_sync) {
          // --- 情况 B: 同步指令立即执行成功 ---
          xSemaphoreTake(cm.mutex, portMAX_DELAY);
          cm.instructions[idx].result_code = 0;
          xQueueSend(cm.result_queue, &idx, 0);
          xSemaphoreGive(cm.mutex);
        }
      }
    }
  }
}

// 2. 结果处理任务 (高优先级，确保回调实时性)
static void comm_result_task(void *params) {
  LOG_DEBUG("comm_result_task");
  int32_t idx;
  while (1) {
    if (xQueueReceive(cm.result_queue, &idx, portMAX_DELAY) == pdTRUE) {
      xSemaphoreTake(cm.mutex, portMAX_DELAY);
      uint32_t err = cm.instructions[idx].result_code;
      instruction_t *inst = &cm.instructions[idx].instruction;

      if (err == 0) {
        if (cm.done_cb)
          cm.done_cb(inst);
      } else {
        if (cm.error_cb)
          cm.error_cb(inst, err);
        cm.instructions[idx].result_code = 0;
      }
      // 释放槽位状态
      cm.instructions[idx].state = COMM_COMPLETED;
      xSemaphoreGive(cm.mutex);
    }
  }
}

// 3. 超时巡检任务 (低频扫描)
static void comm_timeout_task(void *params) {
  LOG_DEBUG("comm_timeout_task");
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(50));
    uint32_t now = xTaskGetTickCount();

    xSemaphoreTake(cm.mutex, portMAX_DELAY);
    for (int i = 0; i < COMM_MAX_INSTRUCTION_NUM; i++) {
      if ((cm.register_bitmap[i >> 3] & (1 << (i & 0x07))) &&
          cm.instructions[i].state == COMM_EXECUTING_ASYNC &&
          cm.instructions[i].timeout != portMAX_DELAY) {

        if ((now - cm.instructions[i].start_time) >
            cm.instructions[i].timeout) {
          cm.instructions[i].result_code = COMM_TIMEOUT_ERROR;
          int32_t idx = i;
          xQueueSend(cm.result_queue, &idx, 0);
        }
      }
    }
    xSemaphoreGive(cm.mutex);
  }
}

/* --- 公共接口实现 --- */

int8_t comm_init(comm_new_instruction_callback new_inst_cb,
                 comm_error_callback error_cb,
                 comm_instruction_done_callback done_cb) {

  memset(&cm, 0, sizeof(cm));
  cm.mutex = xSemaphoreCreateMutex();
  cm.execute_queue = xQueueCreate(COMM_MAX_INSTRUCTION_NUM, sizeof(int32_t));
  cm.result_queue = xQueueCreate(COMM_MAX_INSTRUCTION_NUM, sizeof(int32_t));

  if (!cm.mutex || !cm.execute_queue || !cm.result_queue)
    return -1;

  cm.new_inst_cb = new_inst_cb;
  cm.error_cb = error_cb;
  cm.done_cb = done_cb;

  // 优先级配置：反馈任务 > 执行任务 > 超时任务
  if (xTaskCreate(comm_result_task, "CommRes", 256, NULL, 39, NULL) ==
      pdFALSE) {
    LOG_ERROR("comm_result_task create failed");
    return -1;
  }
  if(xTaskCreate(comm_manager_task, "CommExec", 512, NULL, 38, NULL) ==
     pdFALSE) {
    LOG_ERROR("comm_manager_task create failed");
    return -1;
  }
  if(xTaskCreate(comm_timeout_task, "CommTO", 256, NULL, 36, NULL) ==
     pdFALSE) {
    LOG_ERROR("comm_timeout_task create failed");
    return -1;
  }
  return 0;
}

int32_t comm_register_instruction(uint8_t obj, uint8_t action,
                                  comm_instruction_executor callback,
                                  bool is_sync, uint32_t timeout_ms) {
  if (!cm.mutex)
    return -1;
  xSemaphoreTake(cm.mutex, portMAX_DELAY);

  int32_t idx = find_free_slot();
  if (idx >= 0 && callback) {
    cm.instructions[idx].instruction.obj = obj;
    cm.instructions[idx].instruction.action = action;
    cm.instructions[idx].executor = callback;
    cm.instructions[idx].is_sync = is_sync;
    cm.instructions[idx].timeout =
        (timeout_ms == 0) ? pdMS_TO_TICKS(10) : pdMS_TO_TICKS(timeout_ms);
    cm.instructions[idx].state = COMM_COMPLETED;
    cm.register_bitmap[idx >> 3] |= (1 << (idx & 0x07));
  } else {
    idx = -1;
  }

  xSemaphoreGive(cm.mutex);
  LOG_INFO("AT-%d:OBJ:%d-ACTION:%d", idx, obj, action);
  return idx;
}

int32_t comm_add_instruction_to_execute_queue(instruction_t *inst) {
  int32_t idx = -1;
  if (xSemaphoreTake(cm.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    idx = comm_find_instruction_slot_by_info(inst->obj, inst->action);
    if (idx >= 0 && cm.instructions[idx].state == COMM_COMPLETED) {
      cm.instructions[idx].instruction = *inst;
      cm.instructions[idx].state = COMM_WAITING;
      xQueueSend(cm.execute_queue, &idx, 0);
    } else {
      idx = -1;
    }
    xSemaphoreGive(cm.mutex);
  }
  return idx;
}

int32_t comm_add_instruction_to_execute_queue_from_ISR(instruction_t *inst) {
  BaseType_t yield = pdFALSE;
  int32_t idx = -1;
  if (xSemaphoreTakeFromISR(cm.mutex, &yield) == pdTRUE) {
    idx = comm_find_instruction_slot_by_info(inst->obj, inst->action);
    if (idx >= 0 && cm.instructions[idx].state == COMM_COMPLETED) {
      cm.instructions[idx].instruction = *inst;
      cm.instructions[idx].state = COMM_WAITING;
      xQueueSendFromISR(cm.execute_queue, &idx, &yield);
    } else {
      idx = -1;
    }
    xSemaphoreGiveFromISR(cm.mutex, &yield);
  }
  portYIELD_FROM_ISR(yield);
  return idx;
}

int8_t comm_notify_instruction_done(int32_t index, uint32_t error_code) {
  if (index < 0 || index >= COMM_MAX_INSTRUCTION_NUM)
    return -1;
  xSemaphoreTake(cm.mutex, portMAX_DELAY);
  if (cm.instructions[index].state == COMM_EXECUTING_ASYNC) {
    cm.instructions[index].result_code = error_code;
    xQueueSend(cm.result_queue, &index, 0);
  }
  xSemaphoreGive(cm.mutex);
  return 0;
}

int8_t comm_notify_instruction_done_from_isr(int32_t index,
                                             uint32_t error_code) {
  if (index < 0 || index >= COMM_MAX_INSTRUCTION_NUM)
    return -1;
  BaseType_t yield = pdFALSE;
  xSemaphoreTakeFromISR(cm.mutex, &yield);
  if (cm.instructions[index].state == COMM_EXECUTING_ASYNC) {
    cm.instructions[index].result_code = error_code;
    xQueueSendFromISR(cm.result_queue, &index, &yield);
  }
  xSemaphoreGiveFromISR(cm.mutex, &yield);
  portYIELD_FROM_ISR(yield);
  return 0;
}