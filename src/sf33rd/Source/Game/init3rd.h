#ifndef INIT3RD_H
#define INIT3RD_H

#include "structs.h"
#include "types.h"

void Init_Task(struct _TASK* task_ptr);

/* Derives CC_Value from Country. Called by Init_Task_1st, and by the
 * statcheck harness after it seeds Country from the archive. */
void Setup_Difficult_V();

#endif
