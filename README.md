# Simple-TLV
基于freertos实现的Type-Length-Value的指令管理模块
## 模块设计的基本思想
1. 单片机经常需要执行上层发送的指令，但是如果每条指令都单独if或者switch判断跳转，而且指令之间的互斥关系等。如果只有几条指令是很简单的，但是如果指令数量很多，if/switch就很不友好。最初的想法就是设计一个模块管理所有的指令。
2. 主要将指令分为**同步指令和异步指令**两种：同步指令，单片机接收到这个指令之后，立刻执行并返回，可以立刻知道结果的，异步指令，就是执行需要一定的时间，这个模块并不知道这个指令什么时候结束，所以需要手动触发结束的。
3. 指令的定义是对象-动作-参数，这种形式
## 函数API介绍
1. comm_init:初始化模块，并注册几个回调函数
   - 参数 new_inst_cb:添加新指令触发回调函数，可以为NULL
   - 参数 error_cb:同步指令返回的错误或者同步指令触发错误的回调
   - 参数 done_cb:指令执行结束回调，同步指令和异步指令都会触发
   - 返回值 int8_t:初始化失败返回-1，成功返回0
2. comm_register_instruction：注册需要监控的指令
   - 参数1 obj:指令对象
   - 参数2 action:指令动作
   - 参数3 callback:指令的执行回调函数(这里需要注意，指令回调函数本身禁止阻塞。不然会影响其他指令的调度，所以回调函数建议采用触发或者通知的形式，比如设置某个电机的速度，触发之后就退出，进制阻塞)
   - 参数4 is_sync:是否是同步指令
   - 参数4 timeout_ms:异步指令的超时时间
   - 返回值 int32_t:注册成功，返回每个指令的唯一索引，失败返回-1
3. comm_find_instruction_slot_by_info:查找一个指令的索引
   - 参数1 obj:指令对象
   - 参数2 action:指令动作
   - 返回值 int32_t:成功返回指令的索引，失败返回-1
4. comm_add_instruction_to_execute_queue/comm_add_instruction_to_execute_queue_from_ISR:将一个指令加入执行队列等待执行
   - 参数1 inst:一个指向指令结构体的指针(内部会复制)
   - 返回值 int32_t:成功返回一个这个指令的索引，指令未注册返回-1
5. comm_notify_instruction_done/comm_notify_instruction_done_from_isr：通知异步指令执行结束
   - 参数1 index:指令的索引
   - 参数2 error_code:错误码，0表示成功，非0表示失败，并传递错误码
   - 返回值 int8_t:成功返回0，失败返回-1
  ## 移植
  不依赖于硬件，但是依赖于FreeRTOS环境


