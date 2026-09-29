/**
 * Title 	: ringbuffer.c
 * Author	: haoxirui
 * Date		: 2022.02 @ecovacs
 * Description 	: implement an ring buffer just like kfifo. Please note that
 * the data in the buffer will keep there until them be read out. If the buffer
 * is full, the data could not be put into any more, until the read action free
 * some space.
 *
 * NOTE: This version used for user space.
 *
 */
#include "ringbuffer.h"

#define MEMORY_FULL_BARRIER

#ifdef MEMORY_FULL_BARRIER
static inline void atomic_full_barrier()
{
	__sync_synchronize();
	__asm__ __volatile__(""
						 :
						 :
						 : "memory");
}
#define smp_mb() atomic_full_barrier()
#define smp_rmb() atomic_full_barrier()
#define smp_wmb() atomic_full_barrier()
#else
#include <stdatomic.h>
#endif

static inline __attribute__((const)) int is_power_of_2(unsigned long n)
{
	return (n != 0 && ((n & (n - 1)) == 0));
}

struct ringbuffer *ringbuffer_create(unsigned int size)
{
	struct ringbuffer *ring_buf;

	if (!is_power_of_2(size))
	{
		printf("The buffer size is must be power of 2!\n");
		errno = EINVAL;
		return NULL;
	}

	ring_buf = malloc(sizeof(*ring_buf));
	if (!ring_buf)
	{
		perror("malloc()");
		return NULL;
	}

	ring_buf->data = malloc(size);
	if (!ring_buf->data)
	{
		perror("malloc()");
		free(ring_buf);
		return NULL;
	}

	ring_buf->size = size;
	ring_buf->read_pos = 0;
	ring_buf->write_pos = 0;

	return ring_buf;
}

void ringbuffer_destroy(struct ringbuffer *ring_buf)
{
	if (ring_buf)
	{
		if (ring_buf->data)
		{
			free(ring_buf->data);
			ring_buf->data = NULL;
		}
		free(ring_buf);
		ring_buf = NULL;
	}
}

/**
 * ringbuffer_put - puts some data into the ringbuffer, no locking version
 * @ring_buf: the ringbuffer to be used.
 * @buffer: the data to be added.
 * @len: the length of the data to be added.
 *
 * This function copies at most @len bytes from the @buffer into
 * the ringbuffer depending on the free space, and returns the number of
 * bytes copied.
 *
 * Note that with only one concurrent reader and one concurrent
 * writer, you don't need extra locking to use these functions.
 */
unsigned int ringbuffer_put(struct ringbuffer *ring_buf, const char *buffer, unsigned int len)
{
	unsigned int l;

	len = min(len, ring_buf->size - ring_buf->write_pos + ring_buf->read_pos);

/*
 * Ensure that we sample the ring_buf->read_pos index before we
 * start putting bytes into the ring_buf.
 */
#ifdef MEMORY_FULL_BARRIER
	smp_mb();
#else
	atomic_thread_fence(memory_order_acq_rel);
#endif

	/* first put the data starting from write_pos to buffer end */
	l = min(len, ring_buf->size - (ring_buf->write_pos & (ring_buf->size - 1)));
	memcpy(ring_buf->data + (ring_buf->write_pos & (ring_buf->size - 1)), buffer, l);

	/* then put the rest (if any) at the beginning of the buffer */
	memcpy(ring_buf->data, buffer + l, len - l);

/*
 * Ensure that we add the bytes to the ring_buf before
 * we update the ring_buf->write_pos index.
 */
#ifdef MEMORY_FULL_BARRIER
	smp_wmb();
#else
	atomic_thread_fence(memory_order_release);
#endif

	ring_buf->write_pos += len;

	return len;
}

/**
 *  ringbuffer_get - gets some data from the ringbuffer, no locking version
 *  @ring_buf: the ringbuffer to be used.
 *  @buffer: where the data must be copied.
 *  @len: the size of the destination buffer.
 *
 *  This function copies at most @len bytes from the ringbuffer into the
 *  @buffer and returns the number of copied bytes.
 *
 *  Note that with only one concurrent reader and one concurrent
 *  writer, you don't need extra locking to use these functions.
 */
unsigned int ringbuffer_get(struct ringbuffer *ring_buf, char *buffer, unsigned int len)
{
	unsigned int l;

	len = min(len, ring_buf->write_pos - ring_buf->read_pos);

/*
 * Ensure that we sample the ring_buf->write_pos index before we
 * start removing bytes from the ring_buf.
 */
#ifdef MEMORY_FULL_BARRIER
	smp_rmb();
#else
	atomic_thread_fence(memory_order_acquire);
#endif

	/* first get the data from ring_buf->read_pos until the end of the buffer */
	l = min(len, ring_buf->size - (ring_buf->read_pos & (ring_buf->size - 1)));
	memcpy(buffer, ring_buf->data + (ring_buf->read_pos & (ring_buf->size - 1)), l);

	/* then get the rest (if any) from the beginning of the buffer */
	memcpy(buffer + l, ring_buf->data, len - l);

/*
 * Ensure that we remove the bytes from the ring_buf before
 * we update the ring_buf->read_pos index.
 */
#ifdef MEMORY_FULL_BARRIER
	smp_mb();
#else
	atomic_thread_fence(memory_order_acq_rel);
#endif

	ring_buf->read_pos += len;

	return len;
}

/**
 * ringbuffer_reset - removes the entire ringbuffer content
 * @ring_buf: address of the ringbuffer to be used
 *
 * Note: usage of ringbuffer_reset() is dangerous. It should be only called when the
 * ringbuffer is exclusived locked or when it is secured that no other thread is
 * accessing the fifo.
 */
void ringbuffer_reset(struct ringbuffer *ring_buf)
{
	ring_buf->write_pos = ring_buf->read_pos = 0;
}

unsigned int ringbuffer_is_empty(struct ringbuffer *ring_buf)
{
	return ring_buf->read_pos == ring_buf->write_pos;
}

unsigned int ringbuffer_is_full(struct ringbuffer *ring_buf)
{
	return ring_buf->size == ring_buf->write_pos - ring_buf->read_pos;
}

unsigned int ringbuffer_len(struct ringbuffer *ring_buf)
{
	return ring_buf->write_pos - ring_buf->read_pos;
}

unsigned int ringbuffer_space_left(struct ringbuffer *ring_buf)
{
	return ring_buf->size - ring_buf->write_pos + ring_buf->read_pos;
}
