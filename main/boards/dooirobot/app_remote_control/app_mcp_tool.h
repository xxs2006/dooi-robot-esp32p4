#ifndef APP_MCP_TOOL_H_
#define APP_MCP_TOOL_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化并注册Doly机器人的MCP工具
 *
 * 工具分组：
 * 1. self.display.*  – 屏幕页面切换（眼睛/时钟/番茄钟/招财猫/音乐/天气/说明书）
 * 2. self.camera.*   – 摄像头视觉模式（人脸追踪 / 手势识别）
 * 3. self.action.*   – 动作：list（列出现有场景）、play（播放现有场景）、
 *                      compose（AI 自主编排：内联书写各部件关键帧与时序后执行）
 * 4. self.movement.* – 底盘运动控制
 * 5. 其它           – 网络信息、蓝牙、红外防跌落、系统关机等
 */
void app_mcp_tool_register(void);

#ifdef __cplusplus
}
#endif

#endif // APP_MCP_TOOL_H_
