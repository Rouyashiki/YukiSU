#include "../src/soft_reboot_flow.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <vector>

using ksud::SoftRebootStage;

int main() {
    const std::vector<SoftRebootStage> stages = {
        SoftRebootStage::PrepareWaiter, SoftRebootStage::ResetBoot,   SoftRebootStage::Scripts,
        SoftRebootStage::Stop,          SoftRebootStage::WaitStopped, SoftRebootStage::Cleanup,
        SoftRebootStage::Rebuild,       SoftRebootStage::RestoreHide, SoftRebootStage::Start,
        SoftRebootStage::Services,      SoftRebootStage::WaitBoot,    SoftRebootStage::Complete,
        SoftRebootStage::Verify,
    };
    for (std::size_t failure = 0; failure <= stages.size(); ++failure) {
        for (const bool confirmed : {false, true}) {
            std::vector<SoftRebootStage> called;
            int rollbacks = 0, starts = 0, flag_restores = 0, service_restores = 0;
            ksud::SoftRebootActions actions;
            actions.run = [&](SoftRebootStage stage) {
                called.push_back(stage);
                return failure == stages.size() || stage != stages[failure];
            };
            actions.rollback = [&] { ++rollbacks; };
            actions.recover_start = [&] {
                ++starts;
                return true;
            };
            actions.recover_services = [&] {
                assert(starts == 1 || flag_restores == 1);
                ++service_restores;
                return true;
            };
            actions.cleanup_confirmed = [&] { return confirmed; };
            actions.restore_boot_flag = [&] { ++flag_restores; };
            const auto result = ksud::run_soft_reboot_flow(actions);
            assert(result.ok == (failure == stages.size()));
            assert(called.size() == std::min(failure + 1, stages.size()));
            assert(std::equal(called.begin(), called.end(), stages.begin()));
            if (failure < stages.size())
                assert(result.failed_stage == stages[failure]);
            // A partially started Android must never lose its mounts to rollback.
            assert(rollbacks == (confirmed && (failure == 6 || failure == 7) ? 1 : 0));
            assert(starts == (failure >= 3 && failure <= 8 ? 1 : 0));
            assert(flag_restores == (failure == 1 || failure == 2 ? 1 : 0));
            assert(service_restores == (confirmed && failure >= 2 && failure <= 8 ? 1 : 0));
            assert(result.recovery_services == (service_restores == 1));
        }
    }
    ksud::SoftRebootActions broken_start;
    broken_start.run = [](auto stage) { return stage != SoftRebootStage::WaitStopped; };
    broken_start.rollback = [] { assert(false); };
    broken_start.cleanup_confirmed = [] { return false; };
    broken_start.recover_start = [] { return false; };
    broken_start.recover_services = [] {
        assert(false);
        return false;
    };
    const auto failed = ksud::run_soft_reboot_flow(broken_start);
    assert(!failed.ok && !failed.recovery_started);

    ksud::SoftRebootActions failed_service;
    failed_service.run = [](auto stage) { return stage != SoftRebootStage::Rebuild; };
    failed_service.rollback = [] {};
    failed_service.cleanup_confirmed = [] { return true; };
    failed_service.recover_start = [] { return true; };
    failed_service.recover_services = [] { return false; };
    const auto service_failure = ksud::run_soft_reboot_flow(failed_service);
    assert(!service_failure.ok && service_failure.recovery_started);
    assert(!service_failure.recovery_services);
    assert(service_failure.failed_stage == SoftRebootStage::Rebuild);
    std::cout
        << "soft reboot flow: all stage failures, uncertain operations, and recovery passed\n";
}
