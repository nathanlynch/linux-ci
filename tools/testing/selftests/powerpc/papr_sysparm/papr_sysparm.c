#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <asm/papr-sysparm.h>

#include "utils.h"

enum {
	/* platform-processor-diagnostics-run-mode */
	TOKEN_PROC_DIAG_RUN_MODE     = 42,
	/* Valid run mode values */
	PROC_DIAG_RUN_MODE_DISABLED  = 0,
	PROC_DIAG_RUN_MODE_STAGGERED = 1,
	PROC_DIAG_RUN_MODE_IMMEDIATE = 2,
	PROC_DIAG_RUN_MODE_PERIODIC  = 3,
};

struct papr_get_sysparm_buf {
	__s32 rtas_status;
	__u16 token;
	union {
		__be16 length;
		__u8   data[4002];
	};
};

#define DEVPATH "/dev/papr-sysparm"

static int dev_papr_sysparm_open_close(void)
{
	const int devfd = open(DEVPATH, O_RDONLY);

	SKIP_IF_MSG(devfd < 0 && errno == ENOENT,
		    DEVPATH " not present");

	FAIL_IF(devfd < 0);

	int err = close(devfd);
	FAIL_IF(err != 0);

	return 0;
}

static int sysparm_get_run_mode(void)
{
	const int devfd = open(DEVPATH, O_RDONLY);

	SKIP_IF_MSG(devfd < 0 && errno == ENOENT,
		    DEVPATH " not present");

	FAIL_IF(devfd < 0);

	const int ioctl_res = ioctl(devfd, PAPR_SYSPARM_GET, &gsp);

	int err = close(devfd);
	FAIL_IF(err != 0);

	return 0;
}

int main(void)
{
	int ret = 0;
	ret |= test_harness(dev_papr_sysparm_open_close, "open/close " DEVPATH);
	ret |= test_harness(sysparm_get_run_mode, "get diagnostic run mode");
}
