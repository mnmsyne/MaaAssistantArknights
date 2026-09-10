#include "OperDevelopTask.h"

#include "Task/OperDevelop/OperDevelopTaskProcess.h"

namespace asst
{
OperDevelopTask::OperDevelopTask(const AsstCallback& callback, Assistant* inst) :
    InterfaceTask(callback, inst, TaskType),
    m_process(make_oper_develop_task_process(callback, inst, TaskType))
{
    m_process->set_retry_times(0);
    m_subtasks.emplace_back(m_process);
}

bool OperDevelopTask::set_params(const json::value& params)
{
    return m_process->set_params(params);
}
} // namespace asst
