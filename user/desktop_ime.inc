/* System text composition. Password/session controls are handled before here. */
#include "pinyin.h"
static char ime_composition[49], ime_original[49], ime_prefix[49], ime_committed[64];
static unsigned ime_page;
static ArkPinyinResult ime_candidates;
static int ime_target = -1, text_target = -1;
static char pending_text[512];
static size_t pending_at;
static ArkUIRect ime_bounds, ime_hits[9];
static bool ime_text_target(void) {
    int w = focus();
    if (dialog_kind >= 1 && dialog_kind <= 4)
        return true;
    int a = window_app(w);
    return session_display_editing() || a == 0 || (a == 3 && settings_search_active) ||
           (a == 10 && task_search_active) || (thirdparty_app(w) && thirdparty[w].text_input);
}
static void ime_cancel(void) {
    ime_composition[0] = ime_original[0] = ime_prefix[0] = ime_committed[0] = 0;
    ime_page = 0;
    ime_target = -1;
    memset(&ime_candidates, 0, sizeof ime_candidates);
    dirty = true;
}
static void ime_tick(void) {
    if (!pending_text[0])
        return;
    if (text_target < 0 || !thirdparty_app(text_target) || thirdparty[text_target].closing ||
        !thirdparty[text_target].pid) {
        pending_text[0] = 0;
        pending_at = 0;
        return;
    }
    while (pending_text[pending_at]) {
        const char *p = pending_text + pending_at;
        int cp = utf8_decode(&p);
        ArkEvent e = {.type = ARK_EV_TEXT, .key = cp};
        if (!thirdparty_request_event(text_target, e))
            return;
        pending_at = (size_t)(p - pending_text);
    }
    pending_text[0] = 0;
    pending_at = 0;
}
static void text_dispatch(const char *text) {
    int target = focus();
    if (pending_text[0] && text_target != target) {
        notice("正在提交文字，请稍后切换输入窗口");
        return;
    }
    if (pending_at) {
        memmove(pending_text, pending_text + pending_at, strlen(pending_text + pending_at) + 1);
        pending_at = 0;
    }
    if (strlen(pending_text) + strlen(text) >= sizeof pending_text) {
        notice("文字输入队列已满，请稍后重试");
        return;
    }
    cat(pending_text, sizeof pending_text, text);
    text_target = target;
    ime_tick();
}
static void ime_learning_save(void) {
    ArkPinyinLearned item;
    int slot = ark_pinyin_learn(ime_original, ime_committed, &item);
    if (slot < 0)
        return;
    char key[128] = "/user/input/pinyin/learned/", n[24];
    uint_to_str((unsigned)slot, n);
    cat(key, 128, n);
    ArkRegistryRequest q = {.op = ARK_REG_SET, .type = ARK_REG_BINARY, .length = sizeof item};
    strcopy(q.key, key, 128);
    memcpy(q.value, &item, sizeof item);
    if (ark_registry(&q) < 0)
        notice("文字已输入；个人用词暂时无法保存");
}
static void ime_commit(unsigned choice) {
    if (choice >= ime_candidates.count)
        return;
    ArkPinyinCandidate c = ime_candidates.choices[choice];
    insert_input(c.text);
    cat(ime_committed, sizeof ime_committed, c.text);
    /* Evidence that a native pinyin commit reached an isolated app process:
     * text_dispatch flushed the whole batch through the target's surface. */
    if (text_target >= 0 && thirdparty_app(text_target) && !pending_text[0]) {
        serial_write("[ime] Native Pinyin commit accepted in ");
        serial_write(eng_titles[text_target]);
        serial_write(" process\n");
    }
    unsigned n = (unsigned)strlen(ime_composition), used = c.consumed;
    if (used > n)
        return;
    while (used < n && ime_composition[used] == '\'')
        used++;
    char segment[49];
    memcpy(segment, ime_composition, used);
    segment[used] = 0;
    cat(ime_prefix, sizeof ime_prefix, segment);
    memmove(ime_composition, ime_composition + used, n - used + 1);
    ime_page = 0;
    if (!ime_composition[0]) {
        ime_learning_save();
        ime_cancel();
    } else
        ark_pinyin_query(ime_composition, ime_page, &ime_candidates);
    dirty = true;
}
static bool ime_key(int key) {
    if (key == 9) {
        ime_enabled = !ime_enabled;
        ime_cancel();
        (void)config_integer("/user/input/pinyin/enabled", ime_enabled);
        notice(ime_enabled ? "中文拼音" : "英文输入");
        return true;
    }
    if (!ime_enabled || !ime_text_target())
        return false;
    if (ime_target >= 0 && ime_target != focus())
        ime_cancel();
    size_t n = strlen(ime_composition);
    if (key >= 'A' && key <= 'Z')
        key += 32;
    if (key >= 'a' && key <= 'z') {
        if (n + strlen(ime_prefix) == 48) {
            notice("请先选择候选文字");
            return true;
        }
        if (!n) {
            ime_target = focus();
            ime_committed[0] = ime_prefix[0] = 0;
        }
        ime_composition[n++] = (char)key;
        ime_composition[n] = 0;
        strcopy(ime_original, ime_prefix, 49);
        cat(ime_original, 49, ime_composition);
        ime_page = 0;
    } else if (key == '\'' && n && n < 48) {
        ime_composition[n++] = '\'';
        ime_composition[n] = 0;
        strcopy(ime_original, ime_prefix, 49);
        cat(ime_original, 49, ime_composition);
        ime_page = 0;
    } else if (key == KEY_BACKSPACE && n) {
        ime_composition[n - 1] = 0;
        strcopy(ime_original, ime_prefix, 49);
        cat(ime_original, 49, ime_composition);
        ime_page = 0;
    } else if (n && (key == '[' || key == KEY_PAGE_UP || key == ']' || key == KEY_PAGE_DOWN)) {
        if ((key == '[' || key == KEY_PAGE_UP) && ime_page)
            ime_page--;
        else if ((key == ']' || key == KEY_PAGE_DOWN) && (ime_page + 1) * 9 < ime_candidates.total)
            ime_page++;
    } else if (n && (key == ' ' || key == KEY_ENTER || (key >= '1' && key <= '9'))) {
        if (!ime_candidates.count) {
            insert_input(ime_composition);
            ime_cancel();
        } else
            ime_commit(key >= '1' && key <= '9' ? (unsigned)(key - '1') : 0);
        return true;
    } else if (n && key == KEY_ESCAPE) {
        ime_cancel();
        return true;
    } else
        return false;
    ark_pinyin_query(ime_composition, ime_page, &ime_candidates);
    dirty = true;
    return true;
}
static void ime_draw(void) {
    if (!ime_enabled || !ime_composition[0]) {
        ime_bounds = (ArkUIRect){0};
        return;
    }
    int width = min(sw - 24, 920), height = 151, x = (sw - width) / 2,
        y = sh - (keyboard ? 442 : 253);
    int w = focus();
    if (thirdparty_app(w) && thirdparty[w].text_input) {
        Window *window = &windows[w];
        ArkRect caret = thirdparty[w].caret;
        x = clamp(window->x + 1 + caret.x, 12, sw - width - 12);
        y = clamp(window->y + 54 + caret.y + caret.h + 8, 42, sh - (keyboard ? 442 : 253));
    }
    ime_bounds = (ArkUIRect){x, y, width, height};
    shadow(x, y, width, height);
    glass(x, y, width, height, 16, 235);
    label(x + 15, y + 12, "拼音", accent);
    clip(x + 71, y + 12, ime_composition, width - 153, ink);
    num(x + width - 65, y + 12, ime_page + 1, muted);
    memset(ime_hits, 0, sizeof ime_hits);
    int cell = (width - 30) / 3;
    for (unsigned i = 0; i < ime_candidates.count; i++) {
        int xx = x + 15 + (int)(i % 3) * cell, yy = y + 42 + (int)(i / 3) * 34;
        ime_hits[i] = (ArkUIRect){xx, yy, cell - 4, 32};
        if (in(mx, my, xx, yy, cell - 4, 32))
            rr(xx, yy, cell - 4, 32, 9, accent, 28);
        num(xx + 5, yy + 7, i + 1, accent);
        clip(xx + 23, yy + 7, ime_candidates.choices[i].text, cell - 31, ink);
    }
}
static bool ime_pointer(bool pressed) {
    if (!pressed || !ime_composition[0])
        return false;
    for (unsigned i = 0; i < ime_candidates.count; i++) {
        ArkUIRect r = ime_hits[i];
        if (in(mx, my, r.x, r.y, r.w, r.h)) {
            ime_commit(i);
            return true;
        }
    }
    if (in(mx, my, ime_bounds.x, ime_bounds.y, ime_bounds.w, ime_bounds.h))
        return true;
    ime_cancel();
    return false;
}
static void ime_reset(void) {
    ime_cancel();
    pending_text[0] = 0;
    pending_at = 0;
    text_target = -1;
    ime_enabled = false;
    ark_pinyin_reset_learning();
    if (!session_active())
        return;
    ArkRegistryRequest q = {.op = ARK_REG_GET};
    strcopy(q.key, "/user/input/pinyin/enabled", 128);
    if (ark_registry(&q) >= 0 && q.type == ARK_REG_INTEGER)
        ime_enabled = q.integer != 0;
    for (unsigned i = 0; i < 256; i++) {
        q = (ArkRegistryRequest){.op = ARK_REG_GET};
        strcopy(q.key, "/user/input/pinyin/learned/", 128);
        char n[24];
        uint_to_str(i, n);
        cat(q.key, 128, n);
        if (ark_registry(&q) >= 0 && q.type == ARK_REG_BINARY &&
            q.length == sizeof(ArkPinyinLearned)) {
            ArkPinyinLearned item;
            memcpy(&item, q.value, sizeof item);
            ark_pinyin_restore(&item);
        }
    }
}
