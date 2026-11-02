#pragma once

#include <functional>
#include <string>

namespace ksud {

enum class SoftRebootStage {
    Scripts,
    PrepareWaiter,
    ResetBoot,
    Stop,
    WaitStopped,
    Cleanup,
    Rebuild,
    RestoreHide,
    Start,
    Services,
    WaitBoot,
    Complete,
    Verify
};

inline const char* soft_reboot_stage_name(SoftRebootStage stage) {
    switch (stage) {
    case SoftRebootStage::Scripts:
        return "scripts";
    case SoftRebootStage::PrepareWaiter:
        return "prepare_waiter";
    case SoftRebootStage::ResetBoot:
        return "reset_boot";
    case SoftRebootStage::Stop:
        return "stop";
    case SoftRebootStage::WaitStopped:
        return "wait_stopped";
    case SoftRebootStage::Cleanup:
        return "cleanup";
    case SoftRebootStage::Rebuild:
        return "rebuild";
    case SoftRebootStage::RestoreHide:
        return "restore_hide";
    case SoftRebootStage::Start:
        return "start";
    case SoftRebootStage::Services:
        return "services";
    case SoftRebootStage::WaitBoot:
        return "wait_boot";
    case SoftRebootStage::Complete:
        return "complete";
    case SoftRebootStage::Verify:
        return "verify";
    }
    return "unknown";
}

struct SoftRebootActions {
    std::function<bool(SoftRebootStage)> run;
    std::function<void()> rollback;
    std::function<bool()> recover_start;
    std::function<bool()> recover_services;
    std::function<bool()> cleanup_confirmed;
    std::function<void()> restore_boot_flag;
};

struct SoftRebootResult {
    bool ok = false;
    SoftRebootStage failed_stage = SoftRebootStage::Scripts;
    bool recovery_started = false;
    bool recovery_services = false;
};

inline SoftRebootResult run_soft_reboot_flow(const SoftRebootActions& actions) {
    bool stop_attempted = false;
    bool rebuilding = false;
    bool started = false;
    bool start_attempted = false;
    bool reset_attempted = false;
    bool scripts_attempted = false;
    for (const auto stage :
         {SoftRebootStage::PrepareWaiter, SoftRebootStage::ResetBoot, SoftRebootStage::Scripts,
          SoftRebootStage::Stop, SoftRebootStage::WaitStopped, SoftRebootStage::Cleanup,
          SoftRebootStage::Rebuild, SoftRebootStage::RestoreHide, SoftRebootStage::Start,
          SoftRebootStage::Services, SoftRebootStage::WaitBoot, SoftRebootStage::Complete,
          SoftRebootStage::Verify}) {
        if (stage == SoftRebootStage::Stop)
            stop_attempted = true;
        if (stage == SoftRebootStage::ResetBoot)
            reset_attempted = true;
        if (stage == SoftRebootStage::Scripts)
            scripts_attempted = true;
        if (stage == SoftRebootStage::Rebuild)
            rebuilding = true;
        if (stage == SoftRebootStage::Start)
            start_attempted = true;
        if (!actions.run(stage)) {
            if (reset_attempted && !stop_attempted)
                actions.restore_boot_flag();
            if (rebuilding && !start_attempted && actions.cleanup_confirmed())
                actions.rollback();
            const bool recovered = stop_attempted && !started && actions.recover_start();
            const bool services = scripts_attempted && (!stop_attempted || recovered) &&
                                  actions.cleanup_confirmed() && actions.recover_services();
            return {false, stage, recovered, services};
        }
        if (stage == SoftRebootStage::Start)
            started = true;
    }
    return {true, SoftRebootStage::Verify, false};
}

}  // namespace ksud
