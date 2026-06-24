#include "ui_board_anim.hpp"
#include "ui_theme.hpp"

#include <cstdlib>

/* 示意几何：上方矩形（板）从 y=0 滑到 y=slot_y，停顿后回到 0，循环。 */
struct ui_board_anim_t {
    lv_obj_t *root;     /**< 整体容器 */
    lv_obj_t *board;    /**< 橙色矩形 = 打印板 */
    lv_obj_t *slot;     /**< 灰色框 = 槽位 */
    lv_obj_t *caption;
    lv_obj_t *image;    /**< 真实资源占位，默认隐藏 */
    int32_t   slot_top_y;   /**< 板停泊到槽时的 y 坐标 */
};

static void anim_board_y(void *var, int32_t v)
{
    lv_obj_set_y(static_cast<lv_obj_t *>(var), v);
}

static void board_anim_delete_cb(lv_event_t *e)
{
    auto *self = static_cast<ui_board_anim_t *>(lv_event_get_user_data(e));
    if(!self) {
        return;
    }
    if(self->board) {
        lv_anim_delete(self->board, anim_board_y);
    }
    std::free(self);
}

ui_board_anim_t *ui_board_anim_create(lv_obj_t *parent,
                                      int32_t width, int32_t height)
{
    if(!parent) {
        return nullptr;
    }
    auto *self = static_cast<ui_board_anim_t *>(
        std::calloc(1, sizeof(ui_board_anim_t)));
    if(!self) {
        return nullptr;
    }

    if(width  <= 0) width  = USER_HOME_DEVICE_W / 2;
    if(height <= 0) height = USER_HOME_DEVICE_H / 2;

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, width, height);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    self->root = root;

    /* ---- 槽位（底部偏下，灰色虚化方框） ---- */
    const int32_t board_w = (int32_t)(width * 0.7f);
    const int32_t board_h = (int32_t)(height * 0.20f);
    const int32_t slot_h  = (int32_t)(height * 0.30f);
    const int32_t slot_y  = height - slot_h - USER_PAD_LG;

    lv_obj_t *slot = lv_obj_create(root);
    lv_obj_remove_style_all(slot);
    lv_obj_set_size(slot, board_w + USER_PAD, slot_h);
    lv_obj_set_pos(slot, (width - (board_w + USER_PAD)) / 2, slot_y);
    lv_obj_set_style_bg_color(slot, user_color_panel_alt, 0);
    lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(slot, USER_RADIUS, 0);
    lv_obj_set_style_border_width(slot, 2, 0);
    lv_obj_set_style_border_color(slot, user_color_divider, 0);
    self->slot = slot;

    /* ---- 板（橙色实心圆角矩形，反复下滑） ---- */
    lv_obj_t *board = lv_obj_create(root);
    lv_obj_remove_style_all(board);
    lv_obj_set_size(board, board_w, board_h);
    lv_obj_set_pos(board, (width - board_w) / 2, 0);
    lv_obj_set_style_bg_color(board, user_color_accent, 0);
    lv_obj_set_style_bg_opa(board, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(board, USER_RADIUS, 0);
    lv_obj_set_style_shadow_width(board, 24, 0);
    lv_obj_set_style_shadow_color(board, user_color_accent, 0);
    lv_obj_set_style_shadow_opa(board, LV_OPA_40, 0);
    lv_obj_clear_flag(board, LV_OBJ_FLAG_SCROLLABLE);
    self->board = board;
    self->slot_top_y = slot_y + USER_PAD_SM;

    /* 板的反复下滑动画 */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, board);
    lv_anim_set_exec_cb(&a, anim_board_y);
    lv_anim_set_values(&a, 0, self->slot_top_y);
    lv_anim_set_duration(&a, 1600);
    lv_anim_set_playback_duration(&a, 800);
    lv_anim_set_playback_delay(&a, 600);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);

    /* ---- caption ---- */
    lv_obj_t *cap = lv_label_create(root);
    lv_label_set_text(cap, "Insert print board");
    lv_obj_set_style_text_color(cap, user_color_text_dim, 0);
    lv_obj_set_style_text_font(cap, user_font_caption(), 0);
    lv_obj_align(cap, LV_ALIGN_BOTTOM_MID, 0, 0);
    self->caption = cap;

    /* ---- 真实资源占位（默认隐藏） ---- */
    lv_obj_t *img = lv_image_create(root);
    lv_obj_set_size(img, width, height);
    lv_obj_center(img);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    self->image = img;

    lv_obj_add_event_cb(root, board_anim_delete_cb, LV_EVENT_DELETE, self);

    return self;
}

lv_obj_t *ui_board_anim_get_root(ui_board_anim_t *self)
{
    return self ? self->root : nullptr;
}

void ui_board_anim_set_image(ui_board_anim_t *self, const void *image_src)
{
    if(!self || !image_src || !self->image) {
        return;
    }
    /* 切换到真实资源：隐藏几何示意，显示图片 */
    if(self->board)   lv_obj_add_flag(self->board,   LV_OBJ_FLAG_HIDDEN);
    if(self->slot)    lv_obj_add_flag(self->slot,    LV_OBJ_FLAG_HIDDEN);
    if(self->caption) lv_obj_add_flag(self->caption, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(self->image, image_src);
    lv_obj_remove_flag(self->image, LV_OBJ_FLAG_HIDDEN);
}

void ui_board_anim_destroy(ui_board_anim_t *self)
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
