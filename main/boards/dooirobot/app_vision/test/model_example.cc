#if 0
#include "app_vision.h"
#include "dl_image_jpeg.hpp"
#include "hand_detect.hpp"
#include "hand_gesture_recognition.hpp"
#include "human_face_detect.hpp"
#include "bsp/esp-bsp.h"
#include "dooi_camera.h"

#include "dl_image.hpp" 
#include "driver/ppa.h" 
#include "vision_models_impl.h"

const char *TAG = "app_vision";

#if 0
extern const uint8_t gesture_jpg_start[] asm("_binary_gesture_jpg_start");
extern const uint8_t gesture_jpg_end[] asm("_binary_gesture_jpg_end");

extern const uint8_t human_face_jpg_start[] asm("_binary_human_face_jpg_start");
extern const uint8_t human_face_jpg_end[] asm("_binary_human_face_jpg_end");

void human_face_detect_test(void)
{
    dl::image::jpeg_img_t jpeg_img = {.data = (void *)human_face_jpg_start,
                                      .data_len = (size_t)(human_face_jpg_end - human_face_jpg_start)};
    auto img = dl::image::sw_decode_jpeg(jpeg_img, dl::image::DL_IMAGE_PIX_TYPE_RGB888);

    HumanFaceDetect *detect = new HumanFaceDetect();

    auto &detect_results = detect->run(img);
    for (const auto &res : detect_results) {
        ESP_LOGI(TAG,
                 "[score: %f, x1: %d, y1: %d, x2: %d, y2: %d]",
                 res.score,
                 res.box[0],
                 res.box[1],
                 res.box[2],
                 res.box[3]);
        ESP_LOGI(
            TAG,
            "left_eye: [%d, %d], left_mouth: [%d, %d], nose: [%d, %d], right_eye: [%d, %d], right_mouth: [%d, %d]]",
            res.keypoint[0],
            res.keypoint[1],
            res.keypoint[2],
            res.keypoint[3],
            res.keypoint[4],
            res.keypoint[5],
            res.keypoint[6],
            res.keypoint[7],
            res.keypoint[8],
            res.keypoint[9]);
    }
    delete detect;
    heap_caps_free(img.data);
}

void gesture_detect_test()
{
    dl::image::jpeg_img_t gesture_jpeg = {.data = (void *)gesture_jpg_start,
                                          .data_len = (size_t)(gesture_jpg_end - gesture_jpg_start)};
    auto gesture = dl::image::sw_decode_jpeg(gesture_jpeg, dl::image::DL_IMAGE_PIX_TYPE_RGB888);

    HandDetect *hand_detect = new HandDetect();
    auto hand_gesture_recognizer = new HandGestureRecognizer(HandGestureCls::MOBILENETV2_0_5_S8_V1);
    std::vector<dl::cls::result_t> results = hand_gesture_recognizer->recognize(gesture, hand_detect->run(gesture));

    for (const auto &res : results) {
        ESP_LOGI(TAG, "category: %s, score: %f", res.cat_name, res.score);
    }

    delete hand_detect;
    delete hand_gesture_recognizer;
    heap_caps_free(gesture.data);
}

#endif

#if 0

#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#define HOST_IP_ADDR "172.20.10.4"  // 请修改为你的电脑 IP
#define HOST_PORT    12345

static int sock = -1;

// 初始化 TCP 连接
void tcp_client_init(void)
{
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(HOST_IP_ADDR);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(HOST_PORT);

    while (1) {
        // 1. 创建 TCP Socket (SOCK_STREAM)
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }
        ESP_LOGI(TAG, "Socket created, connecting to %s:%d", HOST_IP_ADDR, HOST_PORT);

        // 2. 连接到 Python 服务器
        int err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err != 0) {
            ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
            close(sock);
            vTaskDelay(2000 / portTICK_PERIOD_MS); // 连接失败重试
            continue;
        }
        
        ESP_LOGI(TAG, "Successfully connected");
        break; // 连接成功，退出初始化循环
    }
}

// TCP 发送函数
void tcp_send_frame(uint8_t *data, uint32_t len)
{
    if (sock < 0) {
        ESP_LOGE(TAG, "Socket not initialized");
        // 如果断开连接，尝试重连的逻辑可以在这里添加，或者在主循环处理
        return;
    }

    // 1. 发送协议头：数据长度 (4字节, 小端序)
    // ESP32 是小端序，Python 默认也能处理，直接发送即可
    int err = send(sock, &len, sizeof(uint32_t), 0);
    if (err < 0) {
        ESP_LOGE(TAG, "Error sending header: errno %d", errno);
        close(sock);
        sock = -1;
        return;
    }

    // 2. 发送图像数据
    // TCP 不需要像 UDP 那样手动分包，但在 LWIP 中发送大块数据(1.9MB)
    // 最好还是循环发送，以免塞满发送缓冲区导致失败
    int to_send = len;
    int sent = 0;
    while (to_send > 0) {
        // 每次发送最大 4KB - 8KB 比较稳妥
        int chunk_size = (to_send > 4096) ? 4096 : to_send;
        
        err = send(sock, data + sent, chunk_size, 0);
        if (err < 0) {
            ESP_LOGE(TAG, "Error sending data: errno %d", errno);
            close(sock);
            sock = -1;
            break;
        }
        to_send -= err;
        sent += err;
    }
}

#endif

class VisionManager {
private:
    std::vector<VisionModelBase*> models;
    int current_model_idx = 0;
    VisionState state = VisionState::IDLE_SCAN;
    ppa_client_handle_t ppa_handle = NULL; 
public:
    void register_model(VisionModelBase* model) {
        model->init(); 
        models.push_back(model);
    }

    void init_ppa() {
        if (ppa_handle != NULL) return;

        ppa_client_config_t ppa_config = {
            .oper_type = PPA_OPERATION_SRM, // SRM = Scaling, Rotate, Mirror
        };
        
        esp_err_t ret = ppa_register_client(&ppa_config, &ppa_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register PPA client: %d", ret);
        } else {
            ESP_LOGI(TAG, "PPA client registered successfully");
        }
    }

    // 核心循环任务
    void app_vision_task() 
    {
        vTaskDelay(10000); 
        // tcp_client_init();

        // init_ppa();
        // int ai_width = 320;
        // int ai_height = 320;
        // uint8_t *ai_buf = (uint8_t *)heap_caps_aligned_alloc(64, ai_width * ai_height * 3, MALLOC_CAP_SPIRAM);
        
        while (1) {
            // --- 调度逻辑 ---
            VisionModelBase* active_model = nullptr;
            FrameBuffer frame = {0};
            
            if (state == VisionState::IDLE_SCAN) {
                active_model = models[current_model_idx];
                current_model_idx = (current_model_idx + 1) % models.size();
                if(current_model_idx == 0)
                {
                    frame = camera_get_face_frame();
                }
                else
                {
                    frame = camera_get_gesture_frame();
                }
            } 
            else if (state == VisionState::FOCUSED) { 
            }
            if (frame.data == nullptr) 
            {
                continue;
            }

            // 输入格式检查
            
            if (!models.empty() && state != VisionState::PAUSED) {
                
                dl::image::img_t img;
                img.data = frame.data;
                img.width = frame.width;
                img.height = frame.height;
                img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888;

                // dl::image::img_t ai_img;
                // ai_img.data = ai_buf;
                // ai_img.width = ai_width;
                // ai_img.height = ai_height;
                // ai_img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888;

                // esp_err_t err = dl::image::resize_ppa(
                //         img, 
                //         ai_img, 
                //         ppa_handle, 
                //         PPA_TRANS_MODE_BLOCKING, 
                //         {}, 
                //         NULL, 
                //         NULL
                // );

                // tcp_send_frame(ai_buf, ai_width * ai_height * 3);

                

                if (active_model) 
                {
                    // ESP_LOGI(TAG, "Running Model: %s", active_model->name());
                    auto results = active_model->run(img);
                    
                    // --- 结果处理 ---
                    if (!results.empty()) 
                    {
                        for(auto &res : results) 
                        {
                            ESP_LOGI(TAG, "Detected: %s, Score: %.2f", res.label, res.score);
                            
                        }
                    }
                }
            }

            vTaskDelay(100); 
        }
    }

    void start() 
    {
        // 放到核心1运行
        xTaskCreatePinnedToCore([](void* arg){ static_cast<VisionManager*>(arg)->app_vision_task(); }, 
                                "vision_task", 8192, this, 1, NULL,1);
    }
};


static VisionManager vision_mgr;

extern "C" void app_vision_init(void) {
#if 0
    gesture_detect_test();
    human_face_detect_test();
#endif

    // 注册需要的模型
    // vision_mgr.register_model(new FaceModel());
    // vision_mgr.register_model(new GestureModel());
    
    // vision_mgr.start();
}
#endif