#include "debugout.h"
#include <qmessagebox.h>

namespace core
{
void output_debug_info(const string& message_head, const string& message_body)
{
    QMessageBox messageBox;
    messageBox.critical(nullptr, message_head.c_str(), message_body.c_str());
    messageBox.setFixedSize(500,200);
}
}
