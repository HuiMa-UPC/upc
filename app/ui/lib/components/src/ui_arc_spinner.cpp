#include "ui_arc_spinner.hpp"
#include "ui_theme.hpp"

#include <cstdlib>

static void spinner_delete_cb(lv_event_t *e)
{
    std::free(lv_event_get_user_data(e));
}

ui_arc_spinner_t *ui_arc_spinner_create(lv_obj_t *parent, int32_t size)
{
    if(!parent) {
        return nullptr;
    }
    auto *self = static_cast<ui_arc_spinner_t *>(
        std::calloc(1, sizeof(ui_arc_spinner_t)));
    if(!self) {
        return nullptr;
    }
    if(size <= 0) {
        size = USER_SPINNER_SZ;
    }

    lv_obj_t *sp = lv_spinner_create(parent);
    lv_obj_set_size(sp, size, size);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_SCROLLABLE);

    /* 一圈 1000ms；弧扫描角度 270°（留 90° 缺口 → "未封闭" 视觉）。
     * lv_spinner 内部让弧起止角以不同速度推进，弧长随之伸缩。 */
    lv_spinner_set_anim_params(sp, 1000, 270);

    /* 底环（淡灰整圈） */
    lv_obj_set_style_arc_color(sp, user_color_divider, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(sp, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_arc_width(sp, USER_SPINNER_STROKE, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(sp, true, LV_PART_MAIN);

    /* 前景弧（主色，扫描段） */
    lv_obj_set_style_arc_color(sp, user_color_accent, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(sp, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(sp, USER_SPINNER_STROKE, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(sp, true, LV_PART_INDICATOR);

    self->root = sp;
    lv_obj_add_event_cb(sp, spinner_delete_cb, LV_EVENT_DELETE, self);
    return self;
}

void ui_arc_spinner_destroy(ui_arc_spinner_t *self)
{
    if(!self) {
        return;
    }
    if(self->root) {
        lv_obj_delete(self->root);
    } else {
        std::free(self);
    }
}
