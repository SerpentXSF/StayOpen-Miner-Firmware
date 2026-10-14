#ifndef MAIN_H_
#define MAIN_H_

void restart_with_reason(const char *reason);
void restart(void);

/* Both defined in main.c. Declared here because the memory census is called
 * from health_maintenance_task rather than from a task of its own -- see the
 * call site for why there is no longer a memory_monitor_task. */
void improved_memory_monitor(void);
void analyze_task_memory(void);

/*
 * Reports the internal pool when the floor moves, and at named points during
 * start-up. Both allocate nothing and run on whatever task calls them, which
 * is the whole design: a 3 KB stack for a monitor task of its own is what
 * stopped asic_result_0 being created once already.
 */
void heap_mark(const char *stage);
bool heap_watermark_moved(void);

#endif /* MAIN_H_ */
