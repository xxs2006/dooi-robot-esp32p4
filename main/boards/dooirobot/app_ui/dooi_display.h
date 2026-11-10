#ifndef CUSTOM_DISPLAY_H
#define CUSTOM_DISPLAY_H

#include "lvgl_display.h"

class DooiDisplay : public LvglDisplay {
public:
    DooiDisplay();
    ~DooiDisplay() override;

   virtual void SetStatus(const char* status) override;
   virtual void ShowNotification(const std::string &notification, int duration_ms = 3000) override;
   virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
   virtual void UpdateStatusBar(bool update_all = false) override;
   virtual void SetEmotion(const char* emotion) override;
   virtual void SetChatMessage(const char* role, const char* content) override;
   virtual void SetTheme(Theme* theme) override;
   virtual void SetPowerSaveMode(bool on) override;
   virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;

   /**
    * @brief 使下一次 SetEmotion 调用被跳过（执行后自动清除）。
    *
    * 用于 self.action.compose 这类自主编排动作：编排已经开始播放原创动作，
    * 其后紧接着的那次 SetEmotion 若被处理会立刻打断该动作，故应被忽略。
    */
   static void ArmSkipNextEmotion(void);
   /** @brief 读取当前是否已武装"跳过下一次表情" */
   static bool IsSkipNextEmotionArmed(void);

protected:
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

private:
    /* 下一次 SetEmotion 是否跳过（compose 动作执行后置位，消费后自动清零） */
    static bool s_skip_next_emotion;
};

#endif // CUSTOM_DISPLAY_H