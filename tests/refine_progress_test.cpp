#include "refine_progress.h"
#include <iostream>

int main()
{
    RefineProgressInfo p;
    const std::string line="@refine 12 633 10 2 2 3 22 40 0.11 0.19 0.2 8 9 \"building with \\\"quotes\\\"\" \"Comparing candidate\" \"top\"";
    if (!ParseRefineProgress(line,p) || p.completed!=12 || p.total!=633 || p.candidate!=22 ||
        p.object!="building with \"quotes\"" || p.worstView!="top" || p.passed!=8) return 1;
    if (ParseRefineProgress("@refine 1 0",p) || p.completed!=12) return 2;
    if (ParseRefineProgress("@progress 1 100 ignored",p)) return 3;
    if (ParseRefineProgress("@refine 4 3 4 0 0 0 0 0 -1 -1 -1 0 0 \"a\" \"b\" \"\"",p)) return 4;
    if (!ParseRefineProgress("@refine 0 10 0 0 0 0 0 0 -1 -1 -1 0 0 \"tree_001\" \"Generating\" \"\"",p)) return 5;
    std::cout << "Refinement progress protocol checks passed.\n";
}
