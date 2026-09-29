/*
  Copyright (c) 2019 Sogou, Inc.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Author: Xie Han (xiehan@sogou-inc.com)
*/

#ifndef _THRDPOOL_H_
#define _THRDPOOL_H_

#include <stddef.h>

typedef struct __thrdpool thrdpool_t;

struct thrdpool_task
{
  void (*routine)(void *);
  void *context;
};

#ifdef __cplusplus
extern "C"
{
#endif

  /*
   * Thread pool originates from project Sogou C++ Workflow
   * https://github.com/sogou/workflow
   *
   * A thread task can be scheduled by another task, which is very important,
   * even if the pool is being destroyed. Because thread task is hard to know
   * what's happening to the pool.
   * The thread pool can also be destroyed by a thread task. This may sound
   * strange, but it's very logical. Destroying thread pool in thread task
   * does not end the task thread. It'll run till the end of task.
   */

  /**************************************************
   *函数名: thrdpool_create
   *功能  : 创建线程池
   *输入  : size_t nthreads : 线程数量
           size_t stacksize : 线程栈大小
   *输出  : 无
   *返回值: 成功 thrdpool_t *，失败 NULL
   *注释  :
  ***************************************************/
  thrdpool_t *thrdpool_create(size_t nthreads, size_t stacksize);

  /**************************************************
   *函数名: thrdpool_schedule
   *功能  : 入任务到线程池
   *输入  : const struct thrdpool_task *task : 线程任务指针
            thrdpool_t *pool : 线程池句柄
   *输出  : 无
   *返回值: 成功 0，失败 -1
   *注释  :
   ***************************************************/
  int thrdpool_schedule(const struct thrdpool_task *task, thrdpool_t *pool);

  /**************************************************
   *函数名: thrdpool_increase
   *功能  : 线程池增加一个线程
   *输入  : thrdpool_t *pool : 线程池句柄
   *输出  : 无
   *返回值: 成功 0，失败 -1
   *注释  :
   ***************************************************/
  int thrdpool_increase(thrdpool_t *pool);

  /**************************************************
   *函数名: thrdpool_in_pool
   *功能  : 线程池有效性判断
   *输入  : thrdpool_t *pool : 线程池句柄
   *输出  : 无
   *返回值: true 或 false
   *注释  :
   ***************************************************/
  int thrdpool_in_pool(thrdpool_t *pool);

  /**************************************************
   *函数名: thrdpool_destroy
   *功能  : 线程池销毁
   *输入  : void (*pending)(const struct thrdpool_task *) : 用户指定函数处理线程池未处理的任务
           thrdpool_t *pool : 线程池句柄
   *输出  : 无
   *返回值: 无
   *注释  :
   ***************************************************/
  void thrdpool_destroy(void (*pending)(const struct thrdpool_task *), thrdpool_t *pool);

#ifdef __cplusplus
}
#endif

#endif
