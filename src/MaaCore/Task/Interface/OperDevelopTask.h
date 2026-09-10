#pragma once

#include "Task/InterfaceTask.h"

#include <memory>

namespace asst
{
class OperDevelopTaskProcess;

class OperDevelopTask final : public InterfaceTask
{
public:
    inline static constexpr std::string_view TaskType = "OperDevelop";

    OperDevelopTask(const AsstCallback& callback, Assistant* inst);
    virtual ~OperDevelopTask() override = default;

    virtual bool set_params(const json::value& params) override;

private:
    std::shared_ptr<OperDevelopTaskProcess> m_process;
};
} // namespace asst
