/* SPDX-License-Identifier: MPL-2.0
 * Three-part touchscreen Stems panel: DRUMS / VOCAL / INST.
 */

#ifndef RX3_STEMS_PANEL_H
#define RX3_STEMS_PANEL_H

static const uint16_t stems_text_drums[] = {
    'D','R','U','M','S',0
};
static const uint16_t stems_text_vocal[] = {
    'V','O','C','A','L',0
};
static const uint16_t stems_text_instrumental[] = {
    'I','N','S','T',0
};

static enum stem_mode stems_control_bit(unsigned int control)
{
    if (control == 0u)
        return MODE_DRUMS;
    if (control == 1u)
        return MODE_VOCAL;
    return MODE_INSTRUMENTAL;
}

static int stems_control_selected(unsigned int deck, unsigned int control)
{
    int armed = stems_decks[deck].reader && stems_decks[deck].armed;
    return armed &&
        (stems_decks[deck].mode & stems_control_bit(control)) != 0;
}

static const uint16_t *stems_panel_label(
    unsigned int deck, unsigned int control)
{
    (void)deck;

    if (control == 0u)
        return stems_text_drums;
    if (control == 1u)
        return stems_text_vocal;
    return stems_text_instrumental;
}

static int stems_panel_selected(unsigned int deck, unsigned int control)
{
    if (deck_is_loading(&stems_decks[deck]))
        return blink_phase_is_on();

    return stems_control_selected(deck, control);
}

static void stems_panel_activate(unsigned int deck, unsigned int control)
{
    if (stems_decks[deck].reader && stems_decks[deck].armed)
        stems_decks[deck].mode = (enum stem_mode)(
            stems_decks[deck].mode ^ stems_control_bit(control)
        );
}

/* Keep requesting the native panel redraw while STEMS is visible. This makes
 * asynchronous audio state and touchscreen state converge visually even when
 * the first glyph invalidation is coalesced by rbp's renderer. */
static int stems_panel_needs_refresh(void)
{
    return 1;
}

static const int stems_panel_lefts[3]  = {19, 215, 411};
static const int stems_panel_rights[3] = {201, 397, 613};

static const struct rx3_panel_feature stems_panel = {
    2u, TAB_IMAGE_STEMS, 3u,
    stems_panel_lefts, stems_panel_rights,
    stems_panel_label, stems_panel_selected, stems_panel_activate,
    stems_panel_needs_refresh
};

#endif /* RX3_STEMS_PANEL_H */
