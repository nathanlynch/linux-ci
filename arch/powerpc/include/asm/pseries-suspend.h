#ifndef PSERIES_SUSPEND_H
#define PSERIES_SUSPEND_H

enum pseries_suspend_state {
	PSERIES_SUSPENDING,
	PSERIES_RESUMING,
};

struct pseries_suspend_handler {
	struct notifier_block notifier_block;
};

void pseries_register_suspend_handler(struct pseries_suspend_handler *h);
void pseries_unregister_suspend_handler(struct pseries_suspend_handler *h);

#endif
