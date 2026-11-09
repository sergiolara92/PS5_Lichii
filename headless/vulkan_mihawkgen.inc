// MihawkGen neural frame generation: the Vulkan present path.
// Included into the generated vulkan_renderer.cpp by prepare-vulkan-port.py, in the
// same place as the LSFG block. Reads the bridge globals (mihawkgen_bridge.h) that
// main.cpp set before the session's renderer started.
#ifdef HAS_MIHAWKGEN
    if (Eden::MihawkGenBridge::active.load(std::memory_order_relaxed)) {
        if (!mihawkgen_init_) {
            mihawkgen_init_ = true;
            Eden::Report("framegen", "MihawkGen: init begin");
            Eden::MihawkGen::Config cfg{};
            cfg.gipa = vkGetInstanceProcAddr;
            cfg.instance = *instance;
            cfg.device = *device.GetLogical();
            cfg.physical_device = device.GetPhysical();
            cfg.width = frame->width;
            cfg.height = frame->height;
            cfg.slots = present_manager.MaxExtraFrames() + 1;
            cfg.output_format = VK_FORMAT_R8G8B8A8_UNORM;
            if (mihawkgen_.Create(cfg)) {
                if (mihawkgen_.LoadWeightsFromFile(
                        Eden::MihawkGenBridge::weights_path.c_str())) {
                    Eden::Report("framegen", "MihawkGen: ready, generating frames");
                } else {
                    Eden::Report("framegen", "MihawkGen: failed to load weights");
                }
            } else {
                Eden::Report("framegen", "MihawkGen: failed to create context");
            }
        }
        if (mihawkgen_.IsReady() && frame->image_view) {
            mihawkgen_.SubmitFrame(*frame->image, *frame->image_view);
            // 0 until two frames are in. Then as many frames as the target rate needs over the time
            // the game's last frames actually took (smoothed; a gap over 250 ms, a load, starts
            // again), so the target holds when the game falls below its frame rate limit: 30 FPS
            // at 120 Hz is three, 24 FPS four. Without a target, the multiplier's. At most what
            // the presenter holds.
            size_t gen_count = mihawkgen_.GenerationCount();
            {
                static std::chrono::steady_clock::time_point last_game_frame{};
                static double frame_ns = 0.0;
                const auto now = std::chrono::steady_clock::now();
                const double gap = std::chrono::duration<double, std::nano>(now - last_game_frame).count();
                last_game_frame = now;
                if (gap > 250e6 || frame_ns == 0.0) {
                    frame_ns = static_cast<double>(
                        VideoCommon::Nx2Perf::g_guest_frame_interval_ns.load(std::memory_order_relaxed));
                    if (frame_ns <= 0.0 && gap <= 250e6) frame_ns = gap;
                } else {
                    frame_ns += (gap - frame_ns) * 0.25;
                }
                if (gen_count != 0) {
                    const long long target = Eden::MihawkGenBridge::target_rate.load(std::memory_order_relaxed);
                    const long long wanted =
                        target > 0 && frame_ns > 0.0
                            ? std::llround(static_cast<double>(target) * frame_ns / 1e9) - 1
                            : Eden::MihawkGenBridge::multiplier.load(std::memory_order_relaxed) - 1;
                    gen_count = static_cast<size_t>(std::clamp<long long>(
                        wanted, target > 0 ? 0 : 1, static_cast<long long>(present_manager.MaxExtraFrames())));
                }
            }
            // Never wait for the display for a made-up frame: the free frames are taken first (one
            // stays for the game's next own frame), and the made-up frames are spaced evenly over
            // as many as there are, so fewer never means uneven motion.
            std::array<Frame*, 8> free_frames{};
            size_t taken = 0;
            while (taken < (std::min)(gen_count, free_frames.size()) &&
                   (free_frames[taken] = present_manager.TryGetRenderFrame(1)) != nullptr) {
                ++taken;
            }
            // Where the made-up frames go, every 5 s in the log (framegen: MihawkGen frames ...).
            static unsigned long long stat_frames = 0, stat_wanted = 0, stat_taken = 0, stat_shown = 0,
                                      stat_no_storage = 0;
            static auto stat_since = std::chrono::steady_clock::now();
            ++stat_frames;
            stat_wanted += gen_count;
            stat_taken += taken;
            gen_count = taken;
            for (size_t g = 0; g < gen_count; ++g) {
                Frame* generated = free_frames[g];
                if (!generated->storage_view) {
                    ++stat_no_storage;
                }
                blit_swapchain.PrepareFrame(device, generated,
                                            render_window.GetFramebufferLayout());
                if (generated->storage_view) {
                    const float t = 1.0f / static_cast<float>(gen_count + 1) *
                                    static_cast<float>(g + 1);
                    scheduler.RequestOutsideRenderPassOperationContext();
                    scheduler.Record(
                        [this, slot = generated->index, t,
                         out_image = *generated->image,
                         out_view = *generated->storage_view](vk::CommandBuffer cmdbuf) {
                            mihawkgen_.RecordGenerate(*cmdbuf, slot, t,
                                                      out_image, out_view);
                        });
                    scheduler.Flush(*generated->render_ready);
                    present_manager.Present(generated);
                    Eden::CountPresentedFrame();
                    ++stat_shown;
                }
            }
            if (const auto now = std::chrono::steady_clock::now(); now - stat_since >= std::chrono::seconds{5}) {
                char line[192];
                std::snprintf(line, sizeof(line),
                              "MihawkGen frames %llu: made-up wanted %llu, got %llu, shown %llu, no storage %llu",
                              stat_frames, stat_wanted, stat_taken, stat_shown, stat_no_storage);
                Eden::Report("framegen", line);
                stat_frames = stat_wanted = stat_taken = stat_shown = stat_no_storage = 0;
                stat_since = now;
            }
        }
    }
#endif
