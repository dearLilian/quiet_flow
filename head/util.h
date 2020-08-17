#pragma once

namespace quiet_flow{
    
enum class RunningStatus{
    Initing,
    Ready,
    Running,
    Yield,
    Finish,
    Fail,
    Recoverable,
};

}