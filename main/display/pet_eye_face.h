#ifndef PET_EYE_FACE_H
#define PET_EYE_FACE_H

#include <lvgl.h>
#include <stdint.h>

class PetEyeFace {
public:
    enum class FaceMode {
        Normal,
        Angry,
        Smile,
        Shy,
        Cry,
        TouchAngry,
    };

    enum class LookMode {
        Auto,
        Center,
        Right,
        Left,
        Track,
    };

    void Create(lv_obj_t* parent, int32_t width, int32_t height);
    void SetFaceMode(FaceMode mode);
    void SetLookMode(LookMode mode);
    void SetFaceTrack(float camera_x);
    void SetFaceTrackPoint(float face_x, float face_y);
    void SetNoFace();
    void SetEmotion(const char* emotion);
    void Update();

private:
    static constexpr int kTouchPupilCanvasW = 86;
    static constexpr int kTouchPupilCanvasH = 64;

    struct Eye {
        lv_obj_t* white = nullptr;
        lv_obj_t* pupil = nullptr;
        lv_obj_t* touch_pupil = nullptr;
        lv_obj_t* glint = nullptr;
        lv_color32_t touch_pupil_buf[kTouchPupilCanvasW * kTouchPupilCanvasH] = {};
        int32_t center_x = 0;
        int32_t center_y = 0;
        int32_t pupil_x = 0;
        int32_t pupil_y = 0;
        uint32_t blink_start_ms = 0;
        int32_t blink_open_percent = 100;
        bool blinking = false;
    };

    lv_obj_t* face_ = nullptr;
    Eye left_;
    Eye right_;
    lv_timer_t* timer_ = nullptr;

    int32_t width_ = 0;
    int32_t height_ = 0;
    int32_t pupil_y_offset_ = 0;
    uint32_t mode_start_ms_ = 0;
    FaceMode mode_ = FaceMode::Normal;
    LookMode look_mode_ = LookMode::Auto;
    float face_track_x_ = 0.0f;
    float face_track_y_ = 0.0f;
    float face_track_target_x_ = 0.0f;
    float face_track_target_y_ = 0.0f;
    uint32_t blink_start_ms_ = 0;
    uint32_t next_blink_ms_ = 0;
    uint32_t blink_second_delay_ms_ = 70;
    uint32_t noface_start_ms_ = 0;
    bool blinking_ = false;
    bool blink_left_first_ = true;
    bool blink_first_started_ = false;
    bool blink_second_started_ = false;
    bool noface_waiting_ = false;
    bool created_ = false;

    static void TimerCallback(lv_timer_t* timer);
    static lv_obj_t* CreateBox(lv_obj_t* parent, int32_t w, int32_t h, lv_color_t color, int32_t radius);
    static void StyleWhite(lv_obj_t* obj);
    static void StylePupil(lv_obj_t* obj);
    static void StyleGlint(lv_obj_t* obj);
    static int32_t RoundToInt(float value);
    static void MoveToward(int32_t* value, int32_t target, int32_t step_divisor);
    static void ApproachFloat(float* value, float target, float rate);
    static float ClampFloat(float value, float min_value, float max_value);
    static int32_t BlinkOpen0To100(uint32_t elapsed);

    void InitEye(Eye* eye, int32_t center_x, int32_t center_y);
    void InitTouchPupil(Eye* eye, bool right_eye);
    void HideTouchPupil(Eye* eye);
    void SetWhite(Eye* eye, int32_t w, int32_t h, int32_t rotation);
    void SetPupil(Eye* eye, int32_t target_x, int32_t target_y, int32_t diameter);
    void SetPupilShape(Eye* eye, int32_t target_x, int32_t target_y,
                       int32_t w, int32_t h, int32_t rotation);
    void SetTouchPupil(Eye* eye, int32_t target_x, int32_t target_y);
    void SetGlint(Eye* eye, int32_t target_x, int32_t target_y, int32_t pupil_w, int32_t pupil_h);
    void ApplyBlink(uint32_t now);
    void DrawNormal(uint32_t now);
    void DrawShy(uint32_t now);
    void DrawTouchAngry(uint32_t now);
    void DrawLook(uint32_t now, int32_t side);
    void DrawTrackedFace(uint32_t now);
    void DrawAngry(uint32_t now);
};

#endif // PET_EYE_FACE_H
