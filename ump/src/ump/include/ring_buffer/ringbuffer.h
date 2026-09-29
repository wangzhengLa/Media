#ifndef RING_BUFFER_H_
#define RING_BUFFER_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define min(x, y) (         \
	{                       \
		typeof(x) _x = (x); \
		typeof(y) _y = (y); \
		(void)(&_x == &_y); \
		_x < _y ? _x : _y;  \
	})

	struct ringbuffer
	{
		void *data;
		unsigned int size;
		unsigned int read_pos;
		unsigned int write_pos;
	};

	/**************************************************
	 *函数名: ringbuffer_create
	 *功能  : 创建环形缓存区
	 *输入  : unsigned int bytes : 缓存大小
	 *输出  : 无
	 *返回值: 成功 struct ringbuffer *，失败 NULL
	 *注释  :
	 ***************************************************/
	struct ringbuffer *ringbuffer_create(unsigned int bytes);

	/**************************************************
	 *函数名: ringbuffer_destroy
	 *功能  : 销毁环形缓存区
	 *输入  : struct ringbuffer *ring_buf : 缓存区句柄指针
	 *输出  : 无
	 *返回值: 无
	 *注释  :
	 ***************************************************/
	void ringbuffer_destroy(struct ringbuffer *ring_buf);

	/**************************************************
	 *函数名: ringbuffer_reset
	 *功能  : 重置环形缓存区
	 *输入  : struct ringbuffer *ring_buf : 缓存区句柄指针
	 *输出  : 无
	 *返回值: 无
	 *注释  :
	 ***************************************************/
	void ringbuffer_reset(struct ringbuffer *ring_buf);

	/**************************************************
	 *函数名: ringbuffer_put
	 *功能  : 入环形缓存区
	 *输入  : struct ringbuffer *ring_buf : 缓存区句柄指针
			 const char *buf : 入缓存区buf指针
			 unsigned int len : 入缓存区buf长度
	 *输出  : 无
	 *返回值: 实际入缓存区大小
	 *注释  :
	 ***************************************************/
	unsigned int ringbuffer_put(struct ringbuffer *ring_buf, const char *buf, unsigned int len);

	/**************************************************
	 *函数名: ringbuffer_get
	 *功能  : 出环形缓存区
	 *输入  : struct ringbuffer *ring_buf : 缓存区句柄指针
			 char *buf : 出缓存区buf指针
			 unsigned int len : 出缓存区buf长度
	 *输出  : 无
	 *返回值: 实际出缓存区大小
	 *注释  :
	 ***************************************************/
	unsigned int ringbuffer_get(struct ringbuffer *ring_buf, char *buf, unsigned int len);

	/**************************************************
	 *函数名: ringbuffer_is_empty
	 *功能  : 环形缓存区是否为空
	 *输入  : struct ringbuffer *ring_buf : 缓存区句柄指针
	 *输出  : 无
	 *返回值: true 或 false
	 *注释  :
	 ***************************************************/
	unsigned int ringbuffer_is_empty(struct ringbuffer *ring_buf);

	/**************************************************
	 *函数名: ringbuffer_is_full
	 *功能  : 环形缓存区是否为满
	 *输入  : struct ringbuffer *ring_buf : 缓存区句柄指针
	 *输出  : 无
	 *返回值: true 或 false
	 *注释  :
	 ***************************************************/
	unsigned int ringbuffer_is_full(struct ringbuffer *ring_buf);

	/**************************************************
	 *函数名: ringbuffer_len
	 *功能  : 环形缓存区占用字节数
	 *输入  : struct ringbuffer *ring_buf : 缓存区句柄指针
	 *输出  : 无
	 *返回值: 实际占用字节数
	 *注释  :
	 ***************************************************/
	unsigned int ringbuffer_len(struct ringbuffer *ring_buf);

	/**************************************************
	 *函数名: ringbuffer_space_left
	 *功能  : 环形缓存区剩余空闲字节数
	 *输入  : struct ringbuffer *ring_buf : 缓存区句柄指针
	 *输出  : 无
	 *返回值: 实际剩余空闲字节数
	 *注释  :
	 ***************************************************/
	unsigned int ringbuffer_space_left(struct ringbuffer *ring_buf);

#ifdef __cplusplus
}
#endif

#endif
