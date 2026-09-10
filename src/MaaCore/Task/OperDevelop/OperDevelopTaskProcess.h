#pragma once

#include "Task/AbstractTask.h"

#include <memory>

namespace asst
{
class OperDevelopTaskProcess : public AbstractTask
{
public:
    OperDevelopTaskProcess(const AsstCallback& callback, Assistant* inst, std::string_view task_chain);
    virtual ~OperDevelopTaskProcess() override = default;

    virtual bool set_params(const json::value& params) = 0;
};

std::shared_ptr<OperDevelopTaskProcess>
    make_oper_develop_task_process(const AsstCallback& callback, Assistant* inst, std::string_view task_chain);
} // namespace asst
