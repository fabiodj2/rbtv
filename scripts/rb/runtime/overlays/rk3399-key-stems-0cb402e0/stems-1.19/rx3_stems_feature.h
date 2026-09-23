/* SPDX-License-Identifier: MPL-2.0
 * Stems implementation of the core runtime-feature lifecycle.
 */

#ifndef RX3_STEMS_FEATURE_H
#define RX3_STEMS_FEATURE_H

static unsigned long hooked_get_stream(void *reader, unsigned long position,
                                       Float2 *output, unsigned long frames)
{
    unsigned long result = original_get_stream(reader, position, output, frames);
    struct stems_deck_context *context = context_for_reader(reader);
    if (!context)
        return result;
    int silent = block_is_silent(output, frames);
    if (!silent && context->drums.data && context->vocal.data &&
        (uint64_t)position + frames <= context->drums.frames &&
        (uint64_t)position + frames <= context->vocal.frames) {
        enum stem_mode selected = context->mode;
        if (selected != MODE_ALL ||
            context->transition_cursor < TRANSITION_FRAMES ||
            context->rendered_mode != MODE_ALL)
            apply_mix(context, position, output, frames, selected);
    }
    return result;
}

static void stems_feature_track_will_load(unsigned int deck, void *reader,
                                          const void *track_info)
{
    (void)reader;
    struct stems_deck_context *context = &stems_decks[deck];
    context->pending_path[0] = '\0';
    context->pending_has_sidecar =
        !sidecar_path_for_track(track_info, context->pending_path,
                                sizeof(context->pending_path)) &&
        sidecar_is_readable(context->pending_path);

    /* The old audio thread may continue until stock load stops it. Detach its
       lookup first; the payload remains allocated until track_did_load. */
    context->reader = 0;
}

static void stems_feature_track_did_load(unsigned int deck, void *reader,
                                         const void *track_info)
{
    (void)track_info;
    struct stems_deck_context *context = &stems_decks[deck];
    int has_sidecar = context->pending_has_sidecar;
    context->generation++;
    context->armed = has_sidecar;
    release_payload(&context->drums);
    release_payload(&context->vocal);
    context->mode = MODE_ALL;
    context->rendered_mode = MODE_ALL;
    context->transition_from = MODE_ALL;
    context->transition_to = MODE_ALL;
    context->transition_cursor = TRANSITION_FRAMES;
    __sync_synchronize();
    context->reader = reader;

    if (!has_sidecar)
        return;
    struct stems_load_request *request = mmap(
        0, 4096u, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (request == MAP_FAILED) {
        log_line("sidecar disabled: request allocation failed");
        return;
    }
    request->context = context;
    request->reader = reader;
    request->generation = context->generation;
    size_t path_length = str_length(context->pending_path) + 1u;
    memcpy(request->path, context->pending_path, path_length);
    pthread_t thread;
    if (!pthread_create(&thread, 0, sidecar_loader, request)) {
        pthread_detach(thread);
        log_number("asynchronous sidecar load started, deck = ", deck + 1u);
    } else {
        munmap(request, 4096u);
        log_line("sidecar disabled: loader thread creation failed");
    }
}

static int stems_feature_configured(void)
{
    return stems_dir != 0;
}

static int stems_feature_install(void)
{
    original_get_stream = (get_stream_fn)install_hook(
        &get_stream_hook, GET_STREAM_AT, get_stream_guard,
        (void *)hooked_get_stream);
    return original_get_stream != 0;
}

static void stems_feature_remove(void)
{
    uninstall_hook(&get_stream_hook);
    original_get_stream = 0;
}

static void stems_feature_destroy_deck(unsigned int deck)
{
    release_payload(&stems_decks[deck].drums);
    release_payload(&stems_decks[deck].vocal);
}

#endif /* RX3_STEMS_FEATURE_H */
