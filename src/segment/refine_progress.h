#pragma once
#include <string>
#include <sstream>
#include <iomanip>
#include <cmath>

struct RefineProgressInfo
{
    int completed=0, total=0, applied=0, kept=0;
    int iteration=0, iterations=0, candidate=0, budget=0;
    double mean=-1, worst=-1, threshold=-1;
    int passed=0, views=0;
    std::string object, phase, worstView;
    double elapsed=0, sinceUpdate=0;
};

inline bool ParseRefineProgress(const std::string& line, RefineProgressInfo& result)
{
    if (line.rfind("@refine ",0)!=0) return false;
    RefineProgressInfo next;
    std::istringstream in(line.substr(8));
    if (!(in >> next.completed >> next.total >> next.applied >> next.kept
          >> next.iteration >> next.iterations >> next.candidate >> next.budget
          >> next.mean >> next.worst >> next.threshold >> next.passed >> next.views
          >> std::quoted(next.object) >> std::quoted(next.phase) >> std::quoted(next.worstView))) return false;
    if (next.completed<0 || next.total<next.completed || next.applied<0 || next.kept<0 ||
        next.applied+next.kept!=next.completed || next.iteration<0 || next.iterations<next.iteration ||
        next.candidate<0 || next.budget<next.candidate || next.passed<0 || next.views<next.passed ||
        !std::isfinite(next.mean) || !std::isfinite(next.worst) || !std::isfinite(next.threshold)) return false;
    result=next;
    return true;
}
