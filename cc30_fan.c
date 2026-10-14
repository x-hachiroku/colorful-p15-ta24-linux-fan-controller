// SPDX-License-Identifier: GPL-2.0-only
/* Focused CC30 auto/max fan-mode bridge. No EC access or probe writes. */
#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/sysfs.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#define CC30_LEASE_SECONDS 60
#define CC30_RETRY_SECONDS 5

static const guid_t cc30_dsm_guid =
	GUID_INIT(0x93f224e4, 0xfbdc, 0x4bbf, 0xad, 0xd6, 0xdb, 0x71, 0xbd, 0xc0, 0xaf, 0xad);

struct cc30_fan {
	struct device *dev;
	acpi_handle handle;
	struct mutex lock;
	struct delayed_work restore_work;
	unsigned long deadline;
	int last_error;
	const char *requested;
	bool touched;
	bool needs_auto;
	bool suspended;
	bool stopping;
};

/* Windows SetDCHU_Data: revision 0, function 0x79, package(buffer[256]).
 * A returned object proves evaluation completed, not that firmware acted.
 */
static int cc30_submit_locked(struct cc30_fan *fan, bool max)
{
	u8 payload[256] = { 0 };
	union acpi_object buffer = {
		.buffer = { .type = ACPI_TYPE_BUFFER, .length = sizeof(payload),
			    .pointer = payload },
	};
	union acpi_object package = {
		.package = { .type = ACPI_TYPE_PACKAGE, .count = 1,
			     .elements = &buffer },
	};
	union acpi_object *result;

	put_unaligned_le32(max ? 0x01000001 : 0x01000000, payload);
	fan->touched = true;
	fan->requested = max ? "max" : "auto";
	result = acpi_evaluate_dsm(fan->handle, &cc30_dsm_guid, 0, 0x79, &package);
	fan->last_error = result ? 0 : -EIO;
	ACPI_FREE(result);
	return fan->last_error;
}

static void cc30_restore_work(struct work_struct *work)
{
	struct cc30_fan *fan = container_of(to_delayed_work(work),
					 struct cc30_fan, restore_work);

	mutex_lock(&fan->lock);
	if (fan->needs_auto && !fan->suspended && !fan->stopping) {
		/* A refresh can race an already running worker waiting for lock. */
		if (time_after(fan->deadline, jiffies)) {
			mod_delayed_work(system_wq, &fan->restore_work,
					 fan->deadline - jiffies);
			goto unlock;
		}
		if (!cc30_submit_locked(fan, false)) {
			fan->needs_auto = false;
			fan->deadline = 0;
		} else {
			dev_err_ratelimited(fan->dev, "automatic fan-mode evaluation failed; retrying\n");
			mod_delayed_work(system_wq, &fan->restore_work,
					 CC30_RETRY_SECONDS * HZ);
		}
	}
unlock:
	mutex_unlock(&fan->lock);
}

static ssize_t mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cc30_fan *fan = dev_get_drvdata(dev);
	ssize_t length;

	mutex_lock(&fan->lock);
	length = sysfs_emit(buf, "%s\n", fan->requested);
	mutex_unlock(&fan->lock);
	return length;
}

static ssize_t mode_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct cc30_fan *fan = dev_get_drvdata(dev);
	bool max;
	int error;

	if (sysfs_streq(buf, "max"))
		max = true;
	else if (sysfs_streq(buf, "auto"))
		max = false;
	else
		return -EINVAL;

	mutex_lock(&fan->lock);
	if (fan->suspended || fan->stopping) {
		mutex_unlock(&fan->lock);
		return -EBUSY;
	}
	/* mod_delayed_work plus this mutex serializes refresh against expiry. */
	error = cc30_submit_locked(fan, max);
	if (max && !error) {
		fan->needs_auto = true;
		fan->deadline = jiffies + CC30_LEASE_SECONDS * HZ;
		mod_delayed_work(system_wq, &fan->restore_work, CC30_LEASE_SECONDS * HZ);
	} else if (error) {
		/* A failed evaluation may still have had side effects. */
		fan->needs_auto = true;
		fan->deadline = jiffies;
		mod_delayed_work(system_wq, &fan->restore_work, CC30_RETRY_SECONDS * HZ);
	} else {
		fan->needs_auto = false;
		fan->deadline = 0;
		cancel_delayed_work(&fan->restore_work);
	}
	mutex_unlock(&fan->lock);
	return error ? error : count;
}
static DEVICE_ATTR(mode, 0644, mode_show, mode_store);

static ssize_t lease_seconds_remaining_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cc30_fan *fan = dev_get_drvdata(dev);
	unsigned long remaining = 0;

	mutex_lock(&fan->lock);
	if (fan->needs_auto && time_after(fan->deadline, jiffies))
		remaining = DIV_ROUND_UP(fan->deadline - jiffies, HZ);
	mutex_unlock(&fan->lock);
	return sysfs_emit(buf, "%lu\n", remaining);
}
static DEVICE_ATTR_RO(lease_seconds_remaining);

static ssize_t last_error_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cc30_fan *fan = dev_get_drvdata(dev);
	int error;

	mutex_lock(&fan->lock);
	error = fan->last_error;
	mutex_unlock(&fan->lock);
	return sysfs_emit(buf, "%d\n", error);
}
static DEVICE_ATTR_RO(last_error);

static struct attribute *cc30_attrs[] = {
	&dev_attr_mode.attr,
	&dev_attr_lease_seconds_remaining.attr,
	&dev_attr_last_error.attr,
	NULL,
};
static const struct attribute_group cc30_group = { .attrs = cc30_attrs };

static int cc30_probe(struct platform_device *pdev)
{
	struct cc30_fan *fan;
	struct acpi_buffer path = { ACPI_ALLOCATE_BUFFER, NULL };
	acpi_handle handle = ACPI_HANDLE(&pdev->dev);
	bool match;

	if (!dmi_match(DMI_SYS_VENDOR, "COLORFUL") ||
	    !dmi_match(DMI_PRODUCT_NAME, "P15 TA 24") ||
	    !dmi_match(DMI_BIOS_VERSION, "1.07.05COLO1"))
		return -ENODEV;
	if (!handle || ACPI_FAILURE(acpi_get_name(handle, ACPI_FULL_PATHNAME, &path)))
		return -ENODEV;
	match = !strcmp(path.pointer, "\\_SB_.DCHU");
	ACPI_FREE(path.pointer);
	if (!match || !acpi_has_method(handle, "_DSM"))
		return -ENODEV;

	fan = devm_kzalloc(&pdev->dev, sizeof(*fan), GFP_KERNEL);
	if (!fan)
		return -ENOMEM;
	fan->dev = &pdev->dev;
	fan->handle = handle;
	fan->requested = "unknown";
	mutex_init(&fan->lock);
	INIT_DELAYED_WORK(&fan->restore_work, cc30_restore_work);
	platform_set_drvdata(pdev, fan);
	return sysfs_create_group(&pdev->dev.kobj, &cc30_group);
}

static void cc30_remove(struct platform_device *pdev)
{
	struct cc30_fan *fan = platform_get_drvdata(pdev);

	sysfs_remove_group(&pdev->dev.kobj, &cc30_group);
	mutex_lock(&fan->lock);
	fan->stopping = true;
	mutex_unlock(&fan->lock);
	cancel_delayed_work_sync(&fan->restore_work);
	mutex_lock(&fan->lock);
	if (fan->touched && cc30_submit_locked(fan, false))
		dev_err(fan->dev, "automatic fan-mode evaluation failed during removal\n");
	mutex_unlock(&fan->lock);
}

static void cc30_shutdown(struct platform_device *pdev)
{
	struct cc30_fan *fan = platform_get_drvdata(pdev);

	mutex_lock(&fan->lock);
	fan->stopping = true;
	mutex_unlock(&fan->lock);
	cancel_delayed_work_sync(&fan->restore_work);
	mutex_lock(&fan->lock);
	if (fan->touched && cc30_submit_locked(fan, false))
		dev_err(fan->dev, "automatic fan-mode evaluation failed during shutdown\n");
	mutex_unlock(&fan->lock);
}

static int cc30_suspend(struct device *dev)
{
	struct cc30_fan *fan = dev_get_drvdata(dev);
	int error = 0;

	mutex_lock(&fan->lock);
	fan->suspended = true;
	mutex_unlock(&fan->lock);
	cancel_delayed_work_sync(&fan->restore_work);
	mutex_lock(&fan->lock);
	if (fan->touched)
		error = cc30_submit_locked(fan, false);
	fan->needs_auto = error != 0;
	fan->deadline = 0;
	/* Refuse suspend if automatic restoration could not be evaluated. */
	if (error) {
		fan->suspended = false;
		mod_delayed_work(system_wq, &fan->restore_work, CC30_RETRY_SECONDS * HZ);
	}
	mutex_unlock(&fan->lock);
	return error;
}

static int cc30_resume(struct device *dev)
{
	struct cc30_fan *fan = dev_get_drvdata(dev);

	mutex_lock(&fan->lock);
	fan->suspended = false;
	if (fan->needs_auto)
		mod_delayed_work(system_wq, &fan->restore_work, 0);
	mutex_unlock(&fan->lock);
	return 0;
}
static DEFINE_SIMPLE_DEV_PM_OPS(cc30_pm, cc30_suspend, cc30_resume);

static const struct acpi_device_id cc30_ids[] = {
	{ "CLV0001", 0 },
	{ },
};
/* Deliberately omit MODULE_DEVICE_TABLE: explicit loading only. */
static struct platform_driver cc30_driver = {
	.probe = cc30_probe,
	.remove = cc30_remove,
	.shutdown = cc30_shutdown,
	.driver = {
		.name = "cc30_fan",
		.acpi_match_table = cc30_ids,
		.pm = pm_sleep_ptr(&cc30_pm),
	},
};
module_platform_driver(cc30_driver);

MODULE_DESCRIPTION("COLORFUL P15 TA 24 CC30 automatic/maximum fan-mode bridge");
MODULE_AUTHOR("CC30 Linux investigation");
MODULE_LICENSE("GPL");
