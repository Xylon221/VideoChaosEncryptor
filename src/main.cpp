#include <iostream>
#include <opencv2/opencv.hpp>
#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <chrono>
#include <map>
#include "SafeQueue.h"
#include <random>
#include "ChaosGenerator.h" 


struct FrameData
{
    cv::Mat frame;
    int frame_index;
};

// 队列 + 互斥锁 + 标志
SafeQueue<FrameData> frameQueue;
SafeQueue<FrameData> processedQueue;
std::mutex queueMutex; 
bool finished = false; // 标记读线程结束

// 读视频 并 放入队列
void readVideoThread(cv::VideoCapture& cap){
    cv::VideoWriter originWriter(
        "/home/orangepi/Work/test/output_origin.avi",
        cv::VideoWriter::fourcc('M','J','P','G'),
        15,
        cv::Size(1280,720)
    );

    if (!cap.isOpened()) {
        std::cout << "Failed to open video" << std::endl;
        finished = true;
    }

    int index = 0;
    const int CAPTURE_SECONDS = 10; //10s的视频
    const int FPS = 15;                       // 摄像头是 15fps
    const int MAX_FRAMES = CAPTURE_SECONDS * FPS;

    while (index < MAX_FRAMES)
    {
        FrameData data;
        if(!cap.read(data.frame)) break;

        originWriter.write(data.frame);

        data.frame_index = index++;
        frameQueue.push(data);
    }

    finished = true;  // 采集结束，通知下游线程退出
    originWriter.release();

}

// ---------------------------处理视频线程-------------------------------------------
std::map<int, cv::Mat> ProcessFrameBuffer;
std::mutex bufferMutex;
int next_index = 0; // 全局顺序索引
// 移除 indexMutex，使用 bufferMutex 保护 next_index
// std::mutex indexMutex; 

void processVideoThread(SafeQueue<FrameData>& processed_queue, bool& finished) {
    // 简单全局 master key
    static const uint32_t master_key = 0xA5A5A5A5u;

    while (true) {
        if (finished && frameQueue.empty()) break;

        // 从输入队列阻塞取帧
        FrameData frame_temp;
        if(!frameQueue.try_pop(frame_temp)){
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // ----------------- 混沌加密处理 -----------------
        if (!frame_temp.frame.empty()) {
            cv::Mat &img = frame_temp.frame;
            
            // 1. 生成基于帧索引的种子
            uint32_t seed = master_key ^ static_cast<uint32_t>(frame_temp.frame_index);

            // 2. 初始化混沌生成器
            ChaosGenerator chaos_rng(seed); 

            // 3. 执行异或操作
            if (img.isContinuous()) {
                size_t totalBytes = img.total() * img.elemSize();
                uint8_t* data = img.data;
                
                // 逐字节生成混沌序列并进行 XOR
                for (size_t i = 0; i < totalBytes; ++i) {
                    data[i] ^= chaos_rng.generate_byte(); 
                }
            } else {
                // 非连续按行处理
                for (int row = 0; row < img.rows; ++row) {
                    uint8_t* rowPtr = img.ptr<uint8_t>(row);
                    size_t rowBytes = static_cast<size_t>(img.cols * img.elemSize());
                    
                    for (size_t i = 0; i < rowBytes; ++i) {
                        rowPtr[i] ^= chaos_rng.generate_byte();
                    }
                }
            }
        }
        // ------------------------------------------------------------

        // 放入全局 map 缓冲，等待按顺序输出
        {
            std::lock_guard<std::mutex> lock(bufferMutex);
            // 保持原有的拷贝逻辑 (不使用 std::move，因为 SafeQueue.h 未优化)
            ProcessFrameBuffer[frame_temp.frame_index] = frame_temp.frame.clone(); 
        }

        // 尝试按顺序输出帧到 processed_queue
        while (true) {
            // 保护 Map 和 next_index
            std::lock_guard<std::mutex> lock(bufferMutex); 
            
            auto it = ProcessFrameBuffer.find(next_index);
            if (it == ProcessFrameBuffer.end()) break;

            FrameData out_frame;
            out_frame.frame_index = next_index;
            // 获取 Map 中存储的帧
            out_frame.frame = it->second; 
            
            processed_queue.push(out_frame);
            ProcessFrameBuffer.erase(it);
            next_index++;
        }
    }
    std::cout << "处理线程退出" << std::endl;
}





// -------------------------视频写线程-----------------------------------------
void writeVideoThread(const std::string& outpath, int width, int height, double fps) {
    cv::VideoWriter writer(outpath, cv::VideoWriter::fourcc('M','J','P','G'), fps, cv::Size(width, height));
    if (!writer.isOpened()) {
        std::cout << "Failed to open VideoWriter" << std::endl;
        return;
    }

    while (true) {
        FrameData data;
        if (processedQueue.try_pop(data)) {
            writer.write(data.frame);
            if(data.frame_index % 100 == 0) {
                std::cout << "正在写入:" << data.frame_index << "帧" << std::endl;
            }
        } else {
            // 检查是否应该退出
            if (finished && processedQueue.empty()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    
    writer.release();
    std::cout << "视频写入完成" << std::endl;
}

int main()
{
    std::string video_path = "/home/orangepi/Work/CHAPT1/test_video.mp4";
    std::string outpath = "/home/orangepi/Work/test/output.avi";
    cv::VideoCapture cap(0);
    if(!cap.isOpened()){
        std::cout << "Failed to open the Video" << std::endl;
        return -1;
    }
    int width = cap.get(cv::CAP_PROP_FRAME_WIDTH);
    int height = cap.get(cv::CAP_PROP_FRAME_HEIGHT);
    double fps = cap.get(cv::CAP_PROP_FPS);

    std::cout << "视频宽: " << width << ", 高: " << height << ", 帧率: " << fps << std::endl;
    
    std::thread reader(readVideoThread, std::ref(cap));

    int numThread = 8;
    std::vector<std::thread> processors;
    for (int i = 0; i < numThread; i++)
    {
        // 传递 processedQueue, finished
        processors.emplace_back(processVideoThread, std::ref(processedQueue), std::ref(finished));
    }
    
    std::thread writer(writeVideoThread, outpath, width, height, fps);
    

    reader.join();

    for(auto& t : processors){
        t.join();
    }

    writer.join();

    std::cout << "处理完成!" << std::endl;
    return 0;
} 