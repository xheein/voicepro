#pragma once

#include <vector>
#include <atomic> // 用于原子操作，确保在没有互斥锁的情况下也能保证线程安全
#include <cstring>

/**
 * 类模板：RingBuffer (环形缓冲区 / 循环缓冲区)
 * 作用：它是本项目中最为关键的“数据中转站”。
 * 
 * 在音频处理中，采集声音的线程（生产者）和播放声音的线程（消费者）往往是不步调一致的。
 * 环形缓冲区允许采集线程不停地往里塞数据，而播放线程根据自己的节奏往外拎数据，
 * 只要缓冲区没被塞满或拎空，两个线程就可以各忙各的，互不干扰。
 * 
 * 并发说明：该类使用了“单生产单消费 (SPSC)”模式下的无锁设计。
 * 即：一个线程写，一个线程读。通过 std::atomic 指针来实现线程间的可见性，性能极高。
 */
template<typename T>
class RingBuffer {
public:
    /**
     * 构造函数
     * @param capacity 缓冲区的总容量（可以存储多少个 T 类型的数据点）
     */
    explicit RingBuffer(size_t capacity) 
        : buffer_(capacity), capacity_(capacity), read_pos_(0), write_pos_(0) {
        /**
         * 注意：由于我们使用的是简单的累加指针（write - read），
         * 为了让取模运算永远有效，真实的指针在不断增加，直到溢出。
         * 虽然 C++ 中无符号长整型溢出是有定义的，但如果是超长期运行，
         * 且容量不是 2 的幂次方，逻辑上需要小心处理。本代码使用 % 满足通用性。
         */
    }

    /**
     * 向缓冲区存入数据 (Push/Write)
     * 由采集线程调用。
     * 
     * @param data 要存入的数据数组指针
     * @param count 想要存入的数据数量
     * @return 如果空间足够并成功存入返回 true，如果空间不足（溢出）则返回 false
     */
    bool push(const T* data, size_t count) {
        // 原子读取当前读写位置
        // memory_order_relaxed: 因为我们只在乎该线程内最新的 write_pos_
        size_t write = write_pos_.load(std::memory_order_relaxed);
        // memory_order_acquire: 必须看到读取线程最新的 read_pos_ 进度
        size_t read = read_pos_.load(std::memory_order_acquire);
        
        // 计算当前还剩下多少空位
        size_t available = capacity_ - (write - read);
        if (count > available) {
            return false; // 缓冲区已满，无法存入这么多的数据（发生丢包）
        }

        // 把数据分摊到对应的“格子”里。
        // 取模运算 (%) 确保了当指针超过数组长度时，会绕回数组开头。
        for (size_t i = 0; i < count; ++i) {
            buffer_[(write + i) % capacity_] = data[i];
        }
        
        /**
         * 更新写入指针。
         * memory_order_release: 确保在更新 write_pos_ 之前，所有的 buffer_ 写入动作都已经完成，
         * 这样另一端的读取线程看到新的 write_pos_ 时，拿到的数据一定是正确的。
         */
        write_pos_.store(write + count, std::memory_order_release);
        return true;
    }

    /**
     * 从缓冲区提取数据 (Pop/Read)
     * 由混音渲染线程调用。
     * 
     * @param data 存放提取出的数据的目标内存地址
     * @param max_count 本次最多想提取多少个数据点
     * @return 返回实际提取到的数据点个数（可能小于 max_count）
     */
    size_t pop(T* data, size_t max_count) {
        size_t read = read_pos_.load(std::memory_order_relaxed);
        // 必须看到写入线程最新的 store(release) 的数据
        size_t write = write_pos_.load(std::memory_order_acquire);
        
        // 计算目前有多少鲜活的数据可以被拎走
        size_t available = write - read;
        size_t count = (max_count < available) ? max_count : available;

        // 如果没有数据，直接返回 0
        if (count == 0) return 0;

        // 提取数据并同样通过取模运算处理环形逻辑
        for (size_t i = 0; i < count; ++i) {
            data[i] = buffer_[(read + i) % capacity_];
        }
        
        /**
         * 更新读取指针，表示这些格子已经被腾空了。
         * memory_order_release: 告诉写入线程，我这些格子已经读完了。
         */
        read_pos_.store(read + count, std::memory_order_release);
        return count;
    }

    /**
     * 获取当前缓冲区中还有多少待读取的数据量
     */
    size_t available() const {
        size_t write = write_pos_.load(std::memory_order_acquire);
        size_t read = read_pos_.load(std::memory_order_relaxed);
        return write - read;
    }

    /**
     * 重置缓冲区（逻辑清空）
     */
    void clear() {
        read_pos_.store(0, std::memory_order_relaxed);
        write_pos_.store(0, std::memory_order_relaxed);
    }

private:
    std::vector<T> buffer_;         // 数据载体
    size_t capacity_;               // 最大存储点数
    std::atomic<size_t> read_pos_;  // 逻辑读取总进度（单调递增，原子的）
    std::atomic<size_t> write_pos_; // 逻辑写入总进度（单调递增，原子的）
};
