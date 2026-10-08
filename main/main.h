#ifndef MAIN_H_
#define MAIN_H_

void restart_with_reason(const char *reason);
void restart(void);

/* Both defined in main.c. Declared here because the memory census is called
 * from health_maintenance_task rather than from a task of its own -- see the
 * call site for why there is no longer a memory_monitor_task. */
void improved_memory_monitor(void);
void analyze_task_memory(void);

#endif /* MAIN_H_ */
