#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <asm/papr-vpd.h>

#include "utils.h"

#define ARRAY_SIZE(x_) (sizeof(x_) / sizeof((x_)[0]))

#define DEVPATH "/dev/papr-vpd"

static int dev_papr_vpd_open_close(void)
{
	const int devfd = open(DEVPATH, O_RDONLY);

	SKIP_IF_MSG(devfd < 0 && errno == ENOENT,
		    DEVPATH " not present");

	FAIL_IF(devfd < 0);
	FAIL_IF(close(devfd) != 0);

	return 0;
}

static int dev_papr_vpd_get_handle_all(void)
{
	const int devfd = open(DEVPATH, O_RDONLY);
	struct papr_vpd_handle handle = {};
	int rc;

	SKIP_IF_MSG(devfd < 0 && errno == ENOENT,
		    DEVPATH " not present");

	FAIL_IF(devfd < 0);

	errno = 0;
	rc = ioctl(devfd, PAPR_VPD_CREATE_HANDLE, &handle);
	FAIL_IF(errno != 0);
	FAIL_IF(rc != 0);
	FAIL_IF(handle.fd < 0);
	FAIL_IF(handle.size == 0);
	FAIL_IF(strcmp(handle.loc_code.str, ""));

	FAIL_IF(close(devfd) != 0);
	void *buf = malloc(handle.size);
	FAIL_IF(!buf);
	ssize_t consumed = read(handle.fd, buf, handle.size);
	FAIL_IF(consumed != handle.size);

	/* Ensure EOF */
	FAIL_IF(read(handle.fd, buf, handle.size) != 0);
	FAIL_IF(close(handle.fd));
	return 0;
}

static int dev_papr_vpd_get_handle_byte_at_a_time(void)
{
	const int devfd = open(DEVPATH, O_RDONLY);
	struct papr_vpd_handle handle = {};
	int rc;

	SKIP_IF_MSG(devfd < 0 && errno == ENOENT,
		    DEVPATH " not present");

	FAIL_IF(devfd < 0);

	errno = 0;
	rc = ioctl(devfd, PAPR_VPD_CREATE_HANDLE, &handle);
	FAIL_IF(errno != 0);
	FAIL_IF(rc != 0);
	FAIL_IF(handle.fd < 0);
	FAIL_IF(handle.size == 0);
	FAIL_IF(strcmp(handle.loc_code.str, ""));

	FAIL_IF(close(devfd) != 0);

	size_t consumed = 0;
	while (1) {
		ssize_t res;
		char c;

		errno = 0;
		res = read(handle.fd, &c, sizeof(c));;
		FAIL_IF(res > sizeof(c));
		FAIL_IF(res < 0);
		FAIL_IF(errno != 0);
		consumed += res;
		if (res == 0)
			break;
	}

	printf("consumed = %zu, handle.size = %u\n", consumed, handle.size);

	FAIL_IF(consumed != handle.size);

	/* Ensure EOF */
	char c;
	FAIL_IF(read(handle.fd, &c, 1) != 0);
	FAIL_IF(close(handle.fd));
	return 0;
}


static int dev_papr_vpd_unterm_loc_code(void)
{
	const int devfd = open(DEVPATH, O_RDONLY);
	struct papr_vpd_handle handle = {};
	int rc;

	SKIP_IF_MSG(devfd < 0 && errno == ENOENT,
		    DEVPATH " not present");

	FAIL_IF(devfd < 0);

	/*
	 * Place a non-null byte in every element of loc_code; the
	 * driver should reject this input.
	 */
	for (size_t i = 0; i < ARRAY_SIZE(handle.loc_code.str); ++i)
		handle.loc_code.str[i] = 'x';

	errno = 0;
	rc = ioctl(devfd, PAPR_VPD_CREATE_HANDLE, &handle);
	FAIL_IF(rc != -1);
	FAIL_IF(errno != EINVAL);

	FAIL_IF(close(devfd) != 0);
	return 0;
}

static int dev_papr_vpd_null_handle(void)
{
	const int devfd = open(DEVPATH, O_RDONLY);
	int rc;

	SKIP_IF_MSG(devfd < 0 && errno == ENOENT,
		    DEVPATH " not present");

	FAIL_IF(devfd < 0);

	errno = 0;
	rc = ioctl(devfd, PAPR_VPD_CREATE_HANDLE, NULL);
	FAIL_IF(rc != -1);
	FAIL_IF(errno != EFAULT);

	FAIL_IF(close(devfd) != 0);
	return 0;
}

static int papr_vpd_close_handle_without_reading(void)
{
	const int devfd = open(DEVPATH, O_RDONLY);
	struct papr_vpd_handle handle = {};
	int rc;

	SKIP_IF_MSG(devfd < 0 && errno == ENOENT,
		    DEVPATH " not present");

	FAIL_IF(devfd < 0);

	errno = 0;
	rc = ioctl(devfd, PAPR_VPD_CREATE_HANDLE, &handle);
	FAIL_IF(errno != 0);
	FAIL_IF(rc != 0);
	FAIL_IF(handle.fd < 0);
	FAIL_IF(handle.size == 0);
	FAIL_IF(strcmp(handle.loc_code.str, ""));

	/* close the handle without reading it */
	FAIL_IF(close(handle.fd) != 0);

	FAIL_IF(close(devfd) != 0);
	return 0;
}

static int papr_vpd_seek_handle(void)
{
	const int devfd = open(DEVPATH, O_RDONLY);
	struct papr_vpd_handle handle = {};
	int rc;

	SKIP_IF_MSG(devfd < 0 && errno == ENOENT,
		    DEVPATH " not present");

	FAIL_IF(devfd < 0);

	errno = 0;
	rc = ioctl(devfd, PAPR_VPD_CREATE_HANDLE, &handle);
	FAIL_IF(errno != 0);
	FAIL_IF(rc != 0);
	FAIL_IF(handle.fd < 0);
	FAIL_IF(handle.size == 0);
	FAIL_IF(strcmp(handle.loc_code.str, ""));

	/*
	 * At least for now, the driver does not support seeking. I
	 * think it could be made to, since the data for each handle
	 * is just an unchanging blob.
	 */
	errno = 0;
	off_t seek_res = lseek(handle.fd, 0, SEEK_SET);
	FAIL_IF(errno != ESPIPE);
	FAIL_IF(seek_res != (off_t)-1);

	errno = 0;
	seek_res = lseek(handle.fd, 0, SEEK_CUR);
	FAIL_IF(errno != ESPIPE);
	FAIL_IF(seek_res != (off_t)-1);

	errno = 0;
	seek_res = lseek(handle.fd, 0, SEEK_END);
	FAIL_IF(errno != ESPIPE);
	FAIL_IF(seek_res != (off_t)-1);

	FAIL_IF(close(devfd) != 0);
	return 0;
}


struct vpd_test {
	int (*function)(void);
	const char *description;
};

static struct vpd_test vpd_tests[] = {
	{
		.function = dev_papr_vpd_open_close,
		.description = "open/close " DEVPATH,
	},
	{
		.function = dev_papr_vpd_unterm_loc_code,
		.description = "ensure EINVAL on unterminated location code",
	},
	{
		.function = dev_papr_vpd_null_handle,
		.description = "ensure EFAULT on bad handle addr",
	},
	{
		.function = dev_papr_vpd_get_handle_all,
		.description = "get handle for all VPD"
	},
	{
		.function = papr_vpd_close_handle_without_reading,
		.description = "close handle without consuming VPD"
	},
	{
		.function = papr_vpd_seek_handle,
		.description = "verify seek behavior on handle fd"
	},
	{
		.function = dev_papr_vpd_get_handle_byte_at_a_time,
		.description = "read all VPD one byte at a time"
	}
};

int main(void)
{
	int ret = 0;

	for (size_t i = 0; i < sizeof(vpd_tests) / sizeof(vpd_tests[0]); i++) {
		const struct vpd_test *t = &vpd_tests[i];

		ret |= test_harness(t->function, t->description);
	}

	return ret;
}
