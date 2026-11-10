#ifndef APP_WEB_BEHAVIOR_H_
#define APP_WEB_BEHAVIOR_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 注册行为资源编辑页（/behavior）及 REST API（/api/behavior/*）。
 *
 * 由 app_webserver.c 在 ctrl_server_start 阶段调用，填充路由表。
 * 目标为 SPIFFS behavior 分区下的 JSON 资源文件（scenes / status / asset/*）。
 */
void app_web_behavior_init(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_WEB_BEHAVIOR_H_ */
