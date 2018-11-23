/*
 * This file provides the ACPI based P-state support. This
 * module works with generic cpufreq infrastructure. Most of
 * the code is based on i386 version
 * (arch/i386/kernel/cpu/cpufreq/acpi-cpufreq.c)
 *
 * Copyright (C) 2005 Intel Corp
 *      Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/pal.h>

#include <linux/acpi.h>
#include <acpi/processor.h>

MODULE_AUTHOR("Venkatesh Pallipadi");
MODULE_DESCRIPTION("ACPI Processor P-States Driver");
MODULE_LICENSE("GPL");


struct cpufreq_acpi_io {
	struct acpi_processor_performance	acpi_data;
	unsigned int				resume;
};

static struct cpufreq_acpi_io	*acpi_io_data[NR_CPUS];

static struct cpufreq_driver acpi_cpufreq_driver;


static int
processor_set_pstate (
	u32	value)
{
	s64 retval;

	pr_debug("processor_set_pstate\n");

	retval = ia64_pal_set_pstate((u64)value);

	if (retval) {
		pr_debug("Failed to set freq to 0x%x, with error 0x%lx\n",
		        value, retval);
		return -ENODEV;
	}
	return (int)retval;
}


static int
processor_get_pstate (
	u32	*value)
{
	u64	pstate_index = 0;
	s64 	retval;

	pr_debug("processor_get_pstate\n");

	retval = ia64_pal_get_pstate(&pstate_index,
	                             PAL_GET_PSTATE_TYPE_INSTANT);
	*value = (u32) pstate_index;

	if (retval)
		pr_debug("Failed to get current freq with "
			"error 0x%lx, idx 0x%x\n", retval, *value);

	return (int)retval;
}


/* To be used only after data->acpi_data is initialized */
static unsigned
extract_clock (
	struct cpufreq_acpi_io *data,
	unsigned value,
	unsigned int cpu)
{
	unsigned long i;

	pr_debug("extract_clock\n");

	for (i = 0; i < data->acpi_data.state_count; i++) {
		if (value == data->acpi_data.states[i].status)
			return data->acpi_data.states[i].core_frequency;
	}
	return data->acpi_data.states[i-1].core_frequency;
}


static unsigned int
processor_get_freq (
	struct cpufreq_acpi_io	*data,
	unsigned int		cpu)
{
	int			ret = 0;
	u32			value = 0;
	cpumask_t		saved_mask;
	unsigned long 		clock_freq;

	pr_debug("processor_get_freq\n");

	saved_mask = current->cpus_allowed;
	set_cpus_allowed_ptr(current, cpumask_of(cpu));
	if (smp_processor_id() != cpu)
		goto migrate_end;

	/* processor_get_pstate gets the instantaneous frequency */
	ret = processor_get_pstate(&value);

	if (ret) {
		set_cpus_allowed_ptr(current, &saved_mask);
		pr_warn("get performance failed with error %d\n", ret);
		ret = 0;
		goto migrate_end;
	}
	clock_freq = extract_clock(data, value, cpu);
	ret = (clock_freq*1000);

migrate_end:
	set_cpus_allowed_ptr(current, &saved_mask);
	return ret;
}


static int
processor_set_freq (
	struct cpufreq_acpi_io	*data,
	struct cpufreq_policy   *policy,
	int			state)
{
	int			ret = 0;
	u32			value = 0;
	cpumask_t		saved_mask;
	int			retval;

	pr_debug("processor_set_freq\n");

	saved_mask = current->cpus_allowed;
	set_cpus_allowed_ptr(current, cpumask_of(policy->cpu));
	if (smp_processor_id() != policy->cpu) {
		retval = -EAGAIN;
		goto migrate_end;
	}

	if (state == data->acpi_data.state) {
		if (unlikely(data->resume)) {
			pr_debug("Called after resume, resetting to P%d\n", state);
			data->resume = 0;
		} else {
			pr_debug("Already at target state (P%d)\n", state);
			retval = 0;
			goto migrate_end;
		}
	}

	pr_debug("Transitioning from P%d to P%d\n",
		data->acpi_data.state, state);

	/*
	 * First we write the target state's 'control' value to the
	 * control_register.
	 */

	value = (u32) data->acpi_data.states[state].control;

	pr_debug("Transitioning to state: 0x%08x\n", value);

	ret = processor_set_pstate(value);
	if (ret) {
		pr_warn("Transition failed with error %d\n", ret);
		retval = -ENODEV;
		goto migrate_end;
	}

	data->acpi_data.state = state;

	retval = 0;

migrate_end:
	set_cpus_allowed_ptr(current, &saved_mask);
	return (retval);
}


static unsigned int
acpi_cpufreq_get (
	unsigned int		cpu)
{
	struct cpufreq_acpi_io *data = acpi_io_data[cpu];

	pr_debug("acpi_cpufreq_get\n");

	return processor_get_freq(data, cpu);
}


static int
acpi_cpufreq_target (
	struct cpufreq_policy   *policy,
	unsigned int index)
{
	return processor_set_freq(acpi_io_data[policy->cpu], policy, index);
}

static int
acpi_cpufreq_cpu_init (
	struct cpufreq_policy   *policy)
{
	unsigned int		i;
	unsigned int		cpu = policy->cpu;
	struct cpufreq_acpi_io	*data;
	unsigned int		result = 0;
	struct cpufreq_frequency_table *freq_table;

	pr_debug("acpi_cpufreq_cpu_init\n");

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return (-ENOMEM);

	acpi_io_data[cpu] = data;

	result = acpi_processor_register_performance(&data->acpi_data, cpu);

	if (result)
		goto err_free;

	/* capability check */
	if (data->acpi_data.state_count <= 1) {
		pr_debug("No P-States\n");
		result = -ENODEV;
		goto err_unreg;
	}

	if ((data->acpi_data.control_register.space_id !=
					ACPI_ADR_SPACE_FIXED_HARDWARE) ||
	    (data->acpi_data.status_register.space_id !=
					ACPI_ADR_SPACE_FIXED_HARDWARE)) {
		pr_debug("Unsupported address space [%d, %d]\n",
			(u32) (data->acpi_data.control_register.space_id),
			(u32) (data->acpi_data.status_register.space_id));
		result = -ENODEV;
		goto err_unreg;
	}

	/* alloc freq_table */
	freq_table = kcalloc(data->acpi_data.state_count + 1,
	                           sizeof(*freq_table),
	                           GFP_KERNEL);
	if (!freq_table) {
		result = -ENOMEM;
		goto err_unreg;
	}

	/* detect transition latency */
	policy->cpuinfo.transition_latency = 0;
	for (i=0; i<data->acpi_data.state_count; i++) {
		if ((data->acpi_data.states[i].transition_latency * 1000) >
		    policy->cpuinfo.transition_latency) {
			policy->cpuinfo.transition_latency =
			    data->acpi_data.states[i].transition_latency * 1000;
		}
	}

	/* table init */
	for (i = 0; i <= data->acpi_data.state_count; i++)
	{
		if (i < data->acpi_data.state_count) {
			freq_table[i].frequency =
			      data->acpi_data.states[i].core_frequency * 1000;
		} else {
			freq_table[i].frequency = CPUFREQ_TABLE_END;
		}
	}

	result = cpufreq_table_validate_and_show(policy, freq_table);
	if (result) {
		goto err_freqfree;
	}

	/* notify BIOS that we exist */
	acpi_processor_notify_smm(THIS_MODULE);

	pr_info("CPU%u - ACPI performance management activated\n", cpu);

	for (i = 0; i < data->acpi_data.state_count; i++)
		pr_debug("     %cP%d: %d MHz, %d mW, %d uS, %d uS, 0x%x 0x%x\n",
			(i == data->acpi_data.state?'*':' '), i,
			(u32) data->acpi_data.states[i].core_frequency,
			(u32) data->acpi_data.states[i].power,
			(u32) data->acpi_data.states[i].transition_latency,
			(u32) data->acpi_data.states[i].bus_master_latency,
			(u32) data->acpi_data.states[i].status,
			(u32) data->acpi_data.states[i].control);

	/* the first call to ->target() should result in us actually
	 * writing something to the appropriate registers. */
	data->resume = 1;

	return (result);

 err_freqfree:
	kfree(freq_table);
 err_unreg:
	acpi_processor_unregister_performance(cpu);
 err_free:
	kfree(data);
	acpi_io_data[cpu] = NULL;

	return (result);
}


static int
acpi_cpufreq_cpu_exit (
	struct cpufreq_policy   *policy)
{
	struct cpufreq_acpi_io *data = acpi_io_data[policy->cpu];

	pr_debug("acpi_cpufreq_cpu_exit\n");

	if (data) {
		acpi_io_data[policy->cpu] = NULL;
		acpi_processor_unregister_performance(policy->cpu);
		kfree(policy->freq_table);
		kfree(data);
	}

	return (0);
}


static struct cpufreq_driver acpi_cpufreq_driver = {
	.verify 	= cpufreq_generic_frequency_table_verify,
	.target_index	= acpi_cpufreq_target,
	.get 		= acpi_cpufreq_get,
	.init		= acpi_cpufreq_cpu_init,
	.exit		= acpi_cpufreq_cpu_exit,
	.name		= "acpi-cpufreq",
	.attr		= cpufreq_generic_attr,
};


static int __init
acpi_cpufreq_init (void)
{
	pr_debug("acpi_cpufreq_init\n");

 	return cpufreq_register_driver(&acpi_cpufreq_driver);
}


static void __exit
acpi_cpufreq_exit (void)
{
	pr_debug("acpi_cpufreq_exit\n");

	cpufreq_unregister_driver(&acpi_cpufreq_driver);
	return;
}


late_initcall(acpi_cpufreq_init);
module_exit(acpi_cpufreq_exit);

