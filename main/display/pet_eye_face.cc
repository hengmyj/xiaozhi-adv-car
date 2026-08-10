#include "pet_eye_face.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {

constexpr int32_t kFaceBaseW = 412;
constexpr int32_t kFaceBaseH = 412;
constexpr int32_t kEyeWhiteD = 138;
constexpr int32_t kPupilD = 82;
constexpr int32_t kFaceTrackDrawYOffset = kEyeWhiteD / 5;
constexpr uint32_t kNoFaceAutoDelayMs = 2500;
constexpr float kFaceTrackInputMax = 0.28f;
constexpr float kFaceTrackCenterX = 160.0f;
constexpr float kFaceTrackCenterY = 90.0f;
constexpr float kFaceTrackMinX = 70.0f;
constexpr float kFaceTrackMaxX = 230.0f;
constexpr float kFaceTrackMinY = 40.0f;
constexpr float kFaceTrackMaxY = 140.0f;
constexpr float kTrackPupilMaxX = 43.0f;
constexpr float kTrackPupilMaxY = 32.0f;
constexpr float kFaceTrackDeadZoneX = 0.05f;
constexpr float kFaceTrackDeadZoneY = 0.06f;
constexpr float kFaceTrackExaggerateX = 1.28f;
constexpr float kPi = 3.14159265358979323846f;

float SmoothWave(uint32_t now, uint32_t period, float phase)
{
    return std::sin((static_cast<float>(now % period) / static_cast<float>(period)) *
                    2.0f * kPi + phase);
}

bool IsOneOf(const char* value, const char* a, const char* b = nullptr,
             const char* c = nullptr, const char* d = nullptr)
{
    if(value == nullptr) return false;
    if(a != nullptr && std::strcmp(value, a) == 0) return true;
    if(b != nullptr && std::strcmp(value, b) == 0) return true;
    if(c != nullptr && std::strcmp(value, c) == 0) return true;
    if(d != nullptr && std::strcmp(value, d) == 0) return true;
    return false;
}

} // namespace

void PetEyeFace::Create(lv_obj_t* parent, int32_t width, int32_t height)
{
    if(parent == nullptr || created_) return;

    width_ = width;
    height_ = height;

    face_ = CreateBox(parent, kFaceBaseW, kFaceBaseH, lv_color_black(), 0);
    lv_obj_center(face_);

    InitEye(&left_, 128, 188);
    InitEye(&right_, 284, 188);

    created_ = true;
    mode_start_ms_ = lv_tick_get();
    Update();
    timer_ = lv_timer_create(TimerCallback, 16, this);
}

void PetEyeFace::SetFaceMode(FaceMode mode)
{
    if(!created_) return;

    if(mode_ != mode) {
        mode_start_ms_ = lv_tick_get();
    }
    mode_ = mode;
    Update();
}

void PetEyeFace::SetLookMode(LookMode mode)
{
    if(!created_) return;

    if(look_mode_ != mode) {
        mode_start_ms_ = lv_tick_get();
    }
    look_mode_ = mode;
    noface_waiting_ = false;
    Update();
}

void PetEyeFace::SetFaceTrack(float camera_x)
{
    if(!created_) return;

    float normalized = ClampFloat(camera_x / kFaceTrackInputMax, -1.0f, 1.0f);
    face_track_target_x_ = -std::tanh(normalized * 1.35f) / std::tanh(1.35f);
    face_track_target_y_ = 0.0f;
    noface_waiting_ = false;
    SetLookMode(LookMode::Track);
}

void PetEyeFace::SetFaceTrackPoint(float face_x, float face_y)
{
    if(!created_) return;

    float normalized_x = face_x >= kFaceTrackCenterX
        ? (face_x - kFaceTrackCenterX) / (kFaceTrackMaxX - kFaceTrackCenterX)
        : (face_x - kFaceTrackCenterX) / (kFaceTrackCenterX - kFaceTrackMinX);
    float normalized_y = face_y >= kFaceTrackCenterY
        ? (face_y - kFaceTrackCenterY) / (kFaceTrackMaxY - kFaceTrackCenterY)
        : (face_y - kFaceTrackCenterY) / (kFaceTrackCenterY - kFaceTrackMinY);

    normalized_x = ClampFloat(normalized_x, -1.0f, 1.0f);
    normalized_y = ClampFloat(normalized_y, -1.0f, 1.0f);
    if(std::fabs(normalized_x) < kFaceTrackDeadZoneX) {
        normalized_x = 0.0f;
    }
    if(std::fabs(normalized_y) < kFaceTrackDeadZoneY) {
        normalized_y = 0.0f;
    }

    float shaped_x = std::tanh(normalized_x * 1.65f) / std::tanh(1.65f);
    float shaped_y = std::tanh(normalized_y * 1.15f) / std::tanh(1.15f);

    face_track_target_x_ = ClampFloat(-shaped_x * kFaceTrackExaggerateX, -1.0f, 1.0f);
    face_track_target_y_ = ClampFloat(shaped_y, -1.0f, 1.0f);
    noface_waiting_ = false;
    SetLookMode(LookMode::Track);
}

void PetEyeFace::SetNoFace()
{
    if(!created_) return;

    if(!noface_waiting_) {
        noface_start_ms_ = lv_tick_get();
        noface_waiting_ = true;
    }

    mode_ = FaceMode::Normal;
    look_mode_ = LookMode::Track;
    Update();
}

void PetEyeFace::SetEmotion(const char* emotion)
{
    if(!created_ || emotion == nullptr) return;

    if(IsOneOf(emotion, "angry", "triangle_exclamation", "circle_xmark", "cloud_slash")) {
        SetLookMode(LookMode::Center);
        SetFaceMode(FaceMode::Angry);
        return;
    }

    if(IsOneOf(emotion, "thinking", "confused")) {
        SetLookMode(LookMode::Left);
        SetFaceMode(FaceMode::Normal);
        return;
    }

    if(IsOneOf(emotion, "cool")) {
        SetLookMode(LookMode::Right);
        SetFaceMode(FaceMode::Normal);
        return;
    }

    if(IsOneOf(emotion, "happy", "laughing", "surprised")) {
        SetLookMode(LookMode::Center);
        SetFaceMode(FaceMode::Smile);
        return;
    }

    if(IsOneOf(emotion, "embarrassed", "sleepy")) {
        SetLookMode(LookMode::Center);
        SetFaceMode(FaceMode::Shy);
        return;
    }

    if(IsOneOf(emotion, "sad")) {
        SetLookMode(LookMode::Center);
        SetFaceMode(FaceMode::Cry);
        return;
    }

    SetLookMode(LookMode::Center);
    SetFaceMode(FaceMode::Normal);
}

void PetEyeFace::Update()
{
    if(!created_) return;

    uint32_t now = lv_tick_get();

    if(noface_waiting_) {
        if(lv_tick_elaps(noface_start_ms_) >= kNoFaceAutoDelayMs) {
            noface_waiting_ = false;
            look_mode_ = LookMode::Auto;
            mode_start_ms_ = now;
        } else {
            DrawTrackedFace(now);
            return;
        }
    }

    if(mode_ == FaceMode::Angry) {
        DrawAngry(now);
        return;
    }

    if(mode_ == FaceMode::TouchAngry) {
        DrawTouchAngry(now);
        return;
    }

    if(mode_ == FaceMode::Shy && look_mode_ != LookMode::Track) {
        DrawShy(now);
        return;
    }

    if(look_mode_ == LookMode::Left) {
        DrawLook(now, -1);
        return;
    }

    if(look_mode_ == LookMode::Right) {
        DrawLook(now, 1);
        return;
    }

    if(look_mode_ == LookMode::Track) {
        DrawTrackedFace(now);
        return;
    }

    DrawNormal(now);
}

void PetEyeFace::TimerCallback(lv_timer_t* timer)
{
    PetEyeFace* self = static_cast<PetEyeFace*>(lv_timer_get_user_data(timer));
    if(self != nullptr) {
        self->Update();
    }
}

lv_obj_t* PetEyeFace::CreateBox(lv_obj_t* parent, int32_t w, int32_t h, lv_color_t color, int32_t radius)
{
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    return obj;
}

void PetEyeFace::StyleWhite(lv_obj_t* obj)
{
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_TRANSP, 0);
}

void PetEyeFace::StylePupil(lv_obj_t* obj)
{
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(obj, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_TRANSP, 0);
}

void PetEyeFace::StyleGlint(lv_obj_t* obj)
{
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_80, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_shadow_color(obj, lv_color_white(), 0);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_40, 0);
    lv_obj_set_style_shadow_width(obj, 8, 0);
    lv_obj_set_style_shadow_spread(obj, 1, 0);
}

int32_t PetEyeFace::RoundToInt(float value)
{
    return static_cast<int32_t>(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

void PetEyeFace::MoveToward(int32_t* value, int32_t target, int32_t step_divisor)
{
    if(value == nullptr || step_divisor <= 0) return;

    int32_t delta = target - *value;
    if(delta > -1 && delta < 1) {
        *value = target;
        return;
    }

    int32_t abs_delta = delta < 0 ? -delta : delta;
    int32_t divisor = step_divisor;
    if(abs_delta > 34) {
        divisor = 2;
    } else if(abs_delta > 20) {
        divisor = 3;
    } else if(abs_delta < 6) {
        divisor = step_divisor + 2;
    }

    int32_t step = delta / divisor;
    if(step == 0) {
        step = delta > 0 ? 1 : -1;
    }
    *value += step;
}

void PetEyeFace::ApproachFloat(float* value, float target, float rate)
{
    if(value == nullptr) return;

    rate = ClampFloat(rate, 0.0f, 1.0f);
    float delta = target - *value;
    if(std::fabs(delta) < 0.001f) {
        *value = target;
        return;
    }
    *value += delta * rate;
}

float PetEyeFace::ClampFloat(float value, float min_value, float max_value)
{
    if(value < min_value) return min_value;
    if(value > max_value) return max_value;
    return value;
}

int32_t PetEyeFace::BlinkOpen0To100(uint32_t elapsed)
{
    if(elapsed < 55U) {
        return 100 - static_cast<int32_t>((elapsed * 92U) / 55U);
    }
    if(elapsed < 115U) {
        return 8;
    }
    if(elapsed < 220U) {
        return 8 + static_cast<int32_t>(((elapsed - 115U) * 92U) / 105U);
    }
    return 100;
}

void PetEyeFace::InitEye(Eye* eye, int32_t center_x, int32_t center_y)
{
    eye->center_x = center_x;
    eye->center_y = center_y;

    eye->white = CreateBox(face_, kEyeWhiteD, kEyeWhiteD, lv_color_white(), LV_RADIUS_CIRCLE);
    StyleWhite(eye->white);

    eye->pupil = CreateBox(face_, kPupilD, kPupilD, lv_color_black(), LV_RADIUS_CIRCLE);
    StylePupil(eye->pupil);

    InitTouchPupil(eye, center_x > kFaceBaseW / 2);

    eye->glint = CreateBox(face_, 13, 13, lv_color_white(), LV_RADIUS_CIRCLE);
    StyleGlint(eye->glint);
}

void PetEyeFace::InitTouchPupil(Eye* eye, bool right_eye)
{
    if(eye == nullptr) return;

    eye->touch_pupil = lv_canvas_create(face_);
    lv_canvas_set_buffer(eye->touch_pupil, eye->touch_pupil_buf,
                         kTouchPupilCanvasW, kTouchPupilCanvasH,
                         LV_COLOR_FORMAT_ARGB8888);
    lv_obj_add_flag(eye->touch_pupil, LV_OBJ_FLAG_HIDDEN);

    constexpr int samples = 4;
    constexpr float sample_step = 1.0f / static_cast<float>(samples);
    constexpr float cx = static_cast<float>(kTouchPupilCanvasW) * 0.5f;
    constexpr float cy = 23.0f;
    constexpr float radius = 35.0f;
    constexpr float radius2 = radius * radius;

    for(int y = 0; y < kTouchPupilCanvasH; ++y) {
        for(int x = 0; x < kTouchPupilCanvasW; ++x) {
            int inside = 0;
            for(int sy = 0; sy < samples; ++sy) {
                for(int sx = 0; sx < samples; ++sx) {
                    float px = static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) * sample_step;
                    float py = static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) * sample_step;
                    float dx = px - cx;
                    float dy = py - cy;
                    float cut_y = right_eye
                        ? 27.0f - px * 0.24f
                        : 6.4f + px * 0.24f;

                    if((dx * dx + dy * dy) <= radius2 && py >= cut_y) {
                        ++inside;
                    }
                }
            }

            lv_color32_t color = {};
            color.blue = 0;
            color.green = 0;
            color.red = 0;
            color.alpha = static_cast<uint8_t>((inside * 255) / (samples * samples));
            eye->touch_pupil_buf[y * kTouchPupilCanvasW + x] = color;
        }
    }
}

void PetEyeFace::HideTouchPupil(Eye* eye)
{
    if(eye == nullptr || eye->touch_pupil == nullptr) return;
    lv_obj_add_flag(eye->touch_pupil, LV_OBJ_FLAG_HIDDEN);
}

void PetEyeFace::SetWhite(Eye* eye, int32_t w, int32_t h, int32_t rotation)
{
    if(eye == nullptr || eye->white == nullptr) return;

    lv_obj_clear_flag(eye->white, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(eye->white, w, h);
    lv_obj_set_pos(eye->white,
                   eye->center_x - (w / 2),
                   eye->center_y - (h / 2));
    lv_obj_set_style_transform_pivot_x(eye->white, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(eye->white, h / 2, 0);
    lv_obj_set_style_transform_rotation(eye->white, rotation, 0);
}

void PetEyeFace::SetPupil(Eye* eye, int32_t target_x, int32_t target_y, int32_t diameter)
{
    SetPupilShape(eye, target_x, target_y, diameter, diameter, 0);
}

void PetEyeFace::SetPupilShape(Eye* eye, int32_t target_x, int32_t target_y,
                               int32_t w, int32_t h, int32_t rotation)
{
    if(eye == nullptr || eye->pupil == nullptr) return;
    HideTouchPupil(eye);

    int32_t draw_w = w;
    int32_t draw_h = h;
    int32_t open_percent = eye->blink_open_percent;
    if(mode_ != FaceMode::Angry && open_percent < 98) {
        draw_h = (h * open_percent) / 100;
        if(draw_h < 5) draw_h = 5;
        draw_w = w + ((100 - open_percent) * 8) / 100;
    }

    int32_t half_travel_y = (kEyeWhiteD - draw_h) / 2;
    int32_t min_target_y = -half_travel_y - pupil_y_offset_;
    int32_t max_target_y = half_travel_y - pupil_y_offset_;
    if(target_y < min_target_y) target_y = min_target_y;
    if(target_y > max_target_y) target_y = max_target_y;

    MoveToward(&eye->pupil_x, target_x, 5);
    MoveToward(&eye->pupil_y, target_y, 5);
    if(eye->pupil_y < min_target_y) eye->pupil_y = min_target_y;
    if(eye->pupil_y > max_target_y) eye->pupil_y = max_target_y;

    lv_obj_clear_flag(eye->pupil, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(eye->pupil, draw_w, draw_h);
    lv_obj_set_pos(eye->pupil,
                   eye->center_x + eye->pupil_x - (draw_w / 2),
                   eye->center_y + pupil_y_offset_ + eye->pupil_y - (draw_h / 2));
    lv_obj_set_style_transform_pivot_x(eye->pupil, draw_w / 2, 0);
    lv_obj_set_style_transform_pivot_y(eye->pupil, draw_h / 2, 0);
    lv_obj_set_style_transform_rotation(eye->pupil, rotation, 0);
    SetGlint(eye, eye->pupil_x, eye->pupil_y, draw_w, draw_h);
}

void PetEyeFace::SetTouchPupil(Eye* eye, int32_t target_x, int32_t target_y)
{
    if(eye == nullptr || eye->touch_pupil == nullptr || eye->pupil == nullptr) return;

    MoveToward(&eye->pupil_x, target_x, 5);
    MoveToward(&eye->pupil_y, target_y, 5);
    lv_obj_add_flag(eye->pupil, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(eye->touch_pupil, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(eye->touch_pupil,
                   eye->center_x + eye->pupil_x - (kTouchPupilCanvasW / 2),
                   eye->center_y + pupil_y_offset_ + eye->pupil_y - (kTouchPupilCanvasH / 2));
}

void PetEyeFace::SetGlint(Eye* eye, int32_t target_x, int32_t target_y,
                          int32_t pupil_w, int32_t pupil_h)
{
    if(eye == nullptr || eye->glint == nullptr) return;

    if(eye->blink_open_percent < 35) {
        lv_obj_add_flag(eye->glint, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    int32_t glint_d = pupil_w > 62 ? 14 : 12;
    int32_t glint_h = pupil_h < pupil_w ? (glint_d * pupil_h) / pupil_w : glint_d;
    if(glint_h < 4) glint_h = 4;

    lv_obj_clear_flag(eye->glint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(eye->glint, glint_d, glint_h);
    lv_obj_set_pos(eye->glint,
                   eye->center_x + target_x - (pupil_w / 4) - (glint_d / 2),
                   eye->center_y + pupil_y_offset_ + target_y - (pupil_h / 4) - (glint_h / 2));
    StyleGlint(eye->glint);
}

void PetEyeFace::ApplyBlink(uint32_t now)
{
    if(next_blink_ms_ == 0) {
        next_blink_ms_ = now + 1200 + static_cast<uint32_t>(std::rand() % 1700);
    }

    if(!blinking_ && static_cast<int32_t>(now - next_blink_ms_) >= 0) {
        blinking_ = true;
        blink_start_ms_ = now;
        blink_second_delay_ms_ = 45 + static_cast<uint32_t>(std::rand() % 75);
        blink_left_first_ = (std::rand() % 2) == 0;
        blink_first_started_ = false;
        blink_second_started_ = false;
        next_blink_ms_ = now + 1500 + static_cast<uint32_t>(std::rand() % 3600);
        left_.blinking = false;
        right_.blinking = false;
        left_.blink_open_percent = 100;
        right_.blink_open_percent = 100;
    }

    if(blinking_ && !blink_first_started_) {
        Eye* first = blink_left_first_ ? &left_ : &right_;
        first->blinking = true;
        first->blink_start_ms = blink_start_ms_;
        blink_first_started_ = true;
    }

    if(blinking_ &&
       !blink_second_started_ &&
       lv_tick_elaps(blink_start_ms_) >= blink_second_delay_ms_) {
        Eye* second = blink_left_first_ ? &right_ : &left_;
        second->blinking = true;
        second->blink_start_ms = now;
        blink_second_started_ = true;
    }

    Eye* eyes[] = { &left_, &right_ };
    bool any_blinking = false;
    for(Eye* eye : eyes) {
        if(!eye->blinking) {
            eye->blink_open_percent = 100;
            continue;
        }

        uint32_t elapsed = lv_tick_elaps(eye->blink_start_ms);
        eye->blink_open_percent = BlinkOpen0To100(elapsed);
        if(elapsed > 220U) {
            eye->blinking = false;
            eye->blink_open_percent = 100;
        } else {
            any_blinking = true;
        }
    }

    if(blinking_) {
        uint32_t elapsed = lv_tick_elaps(blink_start_ms_);
        if(elapsed > blink_second_delay_ms_ + 240U && !any_blinking) {
            blinking_ = false;
        }
    }
}

void PetEyeFace::DrawNormal(uint32_t now)
{
    pupil_y_offset_ = 0;
    SetWhite(&left_, kEyeWhiteD, kEyeWhiteD, 0);
    SetWhite(&right_, kEyeWhiteD, kEyeWhiteD, 0);
    ApplyBlink(now);

    float scan_x = SmoothWave(now, 6200U, 0.2f) * 31.0f +
                   SmoothWave(now, 11300U, 1.1f) * 10.0f;
    float scan_y = SmoothWave(now, 8400U, 1.6f) * 12.0f +
                   SmoothWave(now, 15100U, 0.4f) * 5.0f;
    float left_noise_x = SmoothWave(now, 1270U, 0.1f) * 2.2f +
                         SmoothWave(now, 3100U, 1.7f) * 1.2f;
    float right_noise_x = SmoothWave(now, 1430U, 2.0f) * 2.0f +
                          SmoothWave(now, 2870U, 0.8f) * 1.0f;
    float left_noise_y = SmoothWave(now, 1810U, 2.4f) * 1.6f;
    float right_noise_y = SmoothWave(now, 1970U, 0.6f) * 1.5f;

    int32_t left_x = RoundToInt(ClampFloat(scan_x + left_noise_x, -43.0f, 43.0f));
    int32_t right_x = RoundToInt(ClampFloat(scan_x + right_noise_x, -43.0f, 43.0f));
    int32_t left_y = RoundToInt(ClampFloat(scan_y + left_noise_y, -31.0f, 31.0f));
    int32_t right_y = RoundToInt(ClampFloat(scan_y + right_noise_y, -31.0f, 31.0f));
    int32_t pupil_d = kPupilD + RoundToInt(SmoothWave(now, 7300U, 1.4f) * 3.0f);

    if(mode_ == FaceMode::Smile) {
        left_y += 5;
        right_y += 5;
        pupil_d += 2;
    } else if(mode_ == FaceMode::Shy) {
        left_y += 16;
        right_y += 16;
        left_x -= 14;
        right_x += 14;
        left_x = RoundToInt(ClampFloat(static_cast<float>(left_x), -25.0f, -10.0f));
        right_x = RoundToInt(ClampFloat(static_cast<float>(right_x), 10.0f, 25.0f));
        left_y = RoundToInt(ClampFloat(static_cast<float>(left_y), 14.0f, 22.0f));
        right_y = RoundToInt(ClampFloat(static_cast<float>(right_y), 14.0f, 22.0f));
        pupil_d = 70;
    } else if(mode_ == FaceMode::Cry) {
        left_y += 18;
        right_y += 18;
    }

    SetPupil(&left_, left_x, left_y, pupil_d);
    SetPupil(&right_, right_x, right_y, pupil_d);
}

void PetEyeFace::DrawShy(uint32_t now)
{
    pupil_y_offset_ = 0;
    uint32_t elapsed = lv_tick_elaps(mode_start_ms_);
    float intro = elapsed >= 420U ? 1.0f : static_cast<float>(elapsed) / 420.0f;
    intro = intro * intro * (3.0f - 2.0f * intro);
    float pulse = SmoothWave(now, 1420U, 0.25f);
    float sway = SmoothWave(now, 2360U, 1.4f);
    float sparkle = SmoothWave(now, 780U, 0.8f);
    int32_t peek = RoundToInt((1.0f - intro) * 6.0f);

    SetWhite(&left_, kEyeWhiteD, kEyeWhiteD, 0);
    SetWhite(&right_, kEyeWhiteD, kEyeWhiteD, 0);
    ApplyBlink(now);

    int32_t left_x = -18 + RoundToInt(sway * 3.0f) - peek;
    int32_t right_x = 18 + RoundToInt(sway * 3.0f) + peek;
    int32_t left_y = 15 + RoundToInt(pulse * 2.0f);
    int32_t right_y = 15 + RoundToInt(pulse * 2.0f);
    int32_t pupil_w = 64 + RoundToInt(sparkle * 2.0f);
    int32_t pupil_h = 72 + RoundToInt(pulse * 3.0f);

    SetPupilShape(&left_, left_x, left_y, pupil_w, pupil_h, -18);
    SetPupilShape(&right_, right_x, right_y, pupil_w, pupil_h, 18);
}

void PetEyeFace::DrawTouchAngry(uint32_t now)
{
    pupil_y_offset_ = 0;
    SetWhite(&left_, kEyeWhiteD, kEyeWhiteD, 0);
    SetWhite(&right_, kEyeWhiteD, kEyeWhiteD, 0);
    left_.blinking = false;
    right_.blinking = false;
    left_.blink_open_percent = 100;
    right_.blink_open_percent = 100;

    uint32_t elapsed = lv_tick_elaps(mode_start_ms_);
    float pulse = SmoothWave(now, 980U, 0.25f);
    float twitch = SmoothWave(now, 430U, 1.4f);
    int32_t entry = elapsed >= 240U ? 0 : RoundToInt((1.0f - static_cast<float>(elapsed) / 240.0f) * 8.0f);
    int32_t squeeze = RoundToInt(pulse * 2.0f);
    int32_t glare = (elapsed % 1180U) < 120U ? RoundToInt(twitch * 2.0f) : 0;

    SetTouchPupil(&left_, 14 + squeeze - entry + glare, 14);
    SetTouchPupil(&right_, -14 - squeeze + entry - glare, 14);
    lv_obj_add_flag(left_.glint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(right_.glint, LV_OBJ_FLAG_HIDDEN);
}

void PetEyeFace::DrawLook(uint32_t now, int32_t side)
{
    pupil_y_offset_ = 0;
    SetWhite(&left_, kEyeWhiteD, kEyeWhiteD, 0);
    SetWhite(&right_, kEyeWhiteD, kEyeWhiteD, 0);
    ApplyBlink(now);

    uint32_t elapsed = lv_tick_elaps(mode_start_ms_);
    float settle = elapsed >= 520U ? 1.0f : static_cast<float>(elapsed) / 520.0f;
    settle = settle * settle * (3.0f - 2.0f * settle);

    float bob_phase = (static_cast<float>(now % 2400U) / 2400.0f) * 2.0f * kPi;
    int32_t target_x = RoundToInt(static_cast<float>(side) * (12.0f + 31.0f * settle));
    int32_t left_y = RoundToInt(std::sin(bob_phase) * 4.0f);
    int32_t right_y = RoundToInt(std::sin(bob_phase + 0.45f) * 4.0f);

    SetPupil(&left_, target_x, left_y, kPupilD);
    SetPupil(&right_, target_x, right_y, kPupilD);
}

void PetEyeFace::DrawTrackedFace(uint32_t now)
{
    pupil_y_offset_ = kFaceTrackDrawYOffset;
    SetWhite(&left_, kEyeWhiteD, kEyeWhiteD, 0);
    SetWhite(&right_, kEyeWhiteD, kEyeWhiteD, 0);
    ApplyBlink(now);

    float target_x = ClampFloat(face_track_target_x_, -1.0f, 1.0f);
    float target_y = ClampFloat(face_track_target_y_, -1.0f, 1.0f);
    bool same_side_x = (face_track_x_ * target_x) >= 0.0f;
    bool returning_x = same_side_x && std::fabs(target_x) < std::fabs(face_track_x_);
    float distance_x = std::fabs(target_x - face_track_x_);
    float distance_y = std::fabs(target_y - face_track_y_);
    float tempo_x = 0.90f + SmoothWave(now, 970U, 0.4f) * 0.12f +
                    SmoothWave(now, 2300U, 2.0f) * 0.08f;
    float tempo_y = 0.90f + SmoothWave(now, 1230U, 1.6f) * 0.10f +
                    SmoothWave(now, 2800U, 0.2f) * 0.06f;
    float x_rate = returning_x ? 0.075f : 0.25f;
    float y_rate = std::fabs(target_y) < std::fabs(face_track_y_) ? 0.085f : 0.18f;

    if(distance_x > 0.42f) {
        x_rate *= 1.32f;
    } else if(distance_x < 0.11f) {
        x_rate *= 0.58f;
    }

    if(distance_y > 0.38f) {
        y_rate *= 1.24f;
    } else if(distance_y < 0.10f) {
        y_rate *= 0.62f;
    }

    x_rate = ClampFloat(x_rate * tempo_x, 0.035f, 0.38f);
    y_rate = ClampFloat(y_rate * tempo_y, 0.035f, 0.30f);

    ApproachFloat(&face_track_x_, target_x, x_rate);
    ApproachFloat(&face_track_y_, target_y, y_rate);

    float gaze_x = ClampFloat(face_track_x_, -1.0f, 1.0f);
    float gaze_y = ClampFloat(face_track_y_, -1.0f, 1.0f);
    float gaze_amount = ClampFloat(std::sqrt((gaze_x * gaze_x) + (gaze_y * gaze_y)), 0.0f, 1.0f);

    float micro_scale = 1.0f - (gaze_amount * 0.65f);
    float left_micro_x = (SmoothWave(now, 1490U, 0.3f) * 2.0f +
                          SmoothWave(now, 4300U, 1.2f) * 1.2f) * micro_scale;
    float right_micro_x = (SmoothWave(now, 1660U, 2.1f) * 1.9f +
                           SmoothWave(now, 3900U, 0.7f) * 1.0f) * micro_scale;
    float left_micro_y = (SmoothWave(now, 1770U, 1.8f) * 1.5f +
                          SmoothWave(now, 5200U, 0.5f) * 0.8f) * micro_scale;
    float right_micro_y = (SmoothWave(now, 1910U, 0.9f) * 1.4f +
                           SmoothWave(now, 5600U, 2.0f) * 0.7f) * micro_scale;

    int32_t left_target_x = RoundToInt(ClampFloat(gaze_x * kTrackPupilMaxX + left_micro_x,
                                                  -43.0f, 43.0f));
    int32_t right_target_x = RoundToInt(ClampFloat(gaze_x * kTrackPupilMaxX + right_micro_x,
                                                   -43.0f, 43.0f));
    float up_range_y = kTrackPupilMaxY + static_cast<float>(kFaceTrackDrawYOffset);
    float down_range_y = kTrackPupilMaxY;
    float left_base_y = gaze_y < 0.0f
        ? gaze_y * up_range_y
        : gaze_y * down_range_y;
    float right_base_y = gaze_y < 0.0f
        ? gaze_y * up_range_y
        : gaze_y * down_range_y;
    int32_t left_target_y = RoundToInt(ClampFloat(left_base_y + left_micro_y,
                                                  -up_range_y,
                                                  down_range_y));
    int32_t right_target_y = RoundToInt(ClampFloat(right_base_y + right_micro_y,
                                                   -up_range_y,
                                                   down_range_y));

    if(mode_ == FaceMode::Smile) {
        left_target_y += 5;
        right_target_y += 5;
    } else if(mode_ == FaceMode::Shy) {
        left_target_y += 18;
        right_target_y += 18;
        left_target_x -= 12;
        right_target_x += 12;
        left_target_y += 4;
        left_target_x = RoundToInt(ClampFloat(static_cast<float>(left_target_x), -25.0f, 25.0f));
        right_target_x = RoundToInt(ClampFloat(static_cast<float>(right_target_x), -25.0f, 25.0f));
        left_target_y = RoundToInt(ClampFloat(static_cast<float>(left_target_y), 10.0f, 22.0f));
        right_target_y = RoundToInt(ClampFloat(static_cast<float>(right_target_y), 10.0f, 22.0f));
    } else if(mode_ == FaceMode::Cry) {
        left_target_y += 18;
        right_target_y += 18;
    }

    int32_t pupil_d = kPupilD + RoundToInt((1.0f - gaze_amount) * 5.0f +
                                           SmoothWave(now, 4700U, 0.4f) * 2.0f);
    if(mode_ == FaceMode::Shy) {
        pupil_d = 70 + RoundToInt(SmoothWave(now, 1420U, 0.25f) * 2.0f);
        if(pupil_d < 68) pupil_d = 68;
        if(pupil_d > 72) pupil_d = 72;
    } else {
        if(pupil_d < kPupilD - 3) pupil_d = kPupilD - 3;
        if(pupil_d > kPupilD + 7) pupil_d = kPupilD + 7;
    }

    SetPupil(&left_, left_target_x, left_target_y, pupil_d);
    SetPupil(&right_, right_target_x, right_target_y, pupil_d);
}

void PetEyeFace::DrawAngry(uint32_t now)
{
    pupil_y_offset_ = 0;
    SetWhite(&left_, kEyeWhiteD, kEyeWhiteD, 0);
    SetWhite(&right_, kEyeWhiteD, kEyeWhiteD, 0);

    uint32_t elapsed = lv_tick_elaps(mode_start_ms_);
    float phase = (static_cast<float>(elapsed % 1250U) / 1250.0f) * 2.0f * kPi;
    int32_t push = RoundToInt(std::sin(phase) * 7.0f);
    int32_t clamp = RoundToInt(std::sin(phase * 1.7f) * 4.0f);
    int32_t shake = 0;

    if((elapsed % 780U) < 120U) {
        shake = static_cast<int32_t>((elapsed / 24U) % 3U) - 1;
    }

    SetPupilShape(&left_, 17 + push + shake, 5 + clamp, 66, 42, -180);
    SetPupilShape(&right_, -17 - push - shake, 5 - clamp, 66, 42, 180);
}
