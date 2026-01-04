#ifndef COMM_MANAGER_H
#define COMM_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

/* --- 基础配置 --- */
#define COMM_MAX_INSTRUCTION_NUM 32
#define COMM_INSTRUCTION_ARRAY_SIZE ((COMM_MAX_INSTRUCTION_NUM + 7) / 8)
#define COMM_TIMEOUT_ERROR 0xFFFFFFFF

/* --- 数据类型定义 --- */

// 原始指令结构
typedef struct {
  uint8_t obj;      // 对象标识
  uint8_t action;   // 动作标识
  uint32_t para1;   // 参数1
  uint32_t para2;   // 参数2
  uint8_t para_num; // 参数数量
} instruction_t;

// 回调函数类型定义
typedef uint32_t (*comm_instruction_executor)(const instruction_t *inst);
typedef void (*comm_new_instruction_callback)(const instruction_t *inst);
typedef void (*comm_error_callback)(const instruction_t *inst,
                                    uint32_t error_code);
typedef void (*comm_instruction_done_callback)(const instruction_t *inst);

// 指令状态枚举
typedef enum {
  COMM_NONE = 0,
  COMM_WAITING,         // 在队列中等待被执行
  COMM_EXECUTING_SYNC,  // 同步执行中
  COMM_EXECUTING_ASYNC, // 异步处理中（等待外部 notify）
  COMM_COMPLETED,       // 已完成/槽位空闲
} instruction_states_t;

/* --- 外部接口函数 --- */

// 初始化通信管理器
int8_t comm_init(comm_new_instruction_callback new_inst_cb,
                 comm_error_callback error_cb,
                 comm_instruction_done_callback done_cb);

// 注册指令（obj/action 绑定执行器）
int32_t comm_register_instruction(uint8_t obj, uint8_t action,
                                  comm_instruction_executor callback,
                                  bool is_sync, uint32_t timeout_ms);

// 查找指令槽位
int32_t comm_find_instruction_slot_by_info(uint8_t obj, uint8_t action);

// 将指令加入执行队列
int32_t comm_add_instruction_to_execute_queue(instruction_t *inst);
int32_t comm_add_instruction_to_execute_queue_from_ISR(instruction_t *inst);

// 标记异步指令执行完成
int8_t comm_notify_instruction_done(int32_t index, uint32_t error_code);
int8_t comm_notify_instruction_done_from_isr(int32_t index,
                                             uint32_t error_code);


#pragma region 不是这个模块的 只是作为特殊需求存在的


#endif // COMM_MANAGER_H