/*
 * CPU kernel entry/exit control
 *
 * Copyright (C) 2013 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/acpi.h>
#include <linux/errno.h>
#include <linux/of.h>
#include <linux/string.h>
#include <asm/acpi.h>
#include <asm/cpu_ops.h>
#include <asm/smp_plat.h>

extern const struct cpu_operations smp_spin_table_ops;
extern const struct cpu_operations acpi_parking_protocol_ops;
extern const struct cpu_operations cpu_psci_ops;

const struct cpu_operations *cpu_ops[NR_CPUS];

static const struct cpu_operations *dt_supported_cpu_ops[] __initconst = {
	&smp_spin_table_ops,
	&cpu_psci_ops,
	NULL,
};

static const struct cpu_operations *acpi_supported_cpu_ops[] __initconst = {
#ifdef CONFIG_ARM64_ACPI_PARKING_PROTOCOL
	&acpi_parking_protocol_ops,
#endif
	&cpu_psci_ops,
	NULL,
};

static const struct cpu_operations * __init cpu_get_ops(const char *name)
{
	/**
	 * @brief cpu 오퍼레이션을 수행하기 위한 cpu_operation 구조체를 리턴한다.
	 * @param[out] name operation name
	 * @return
	 * 	Pass - cpu_operations*
	 * 	Fail - null
	 */
	const struct cpu_operations **ops;

	ops = acpi_disabled ? dt_supported_cpu_ops : acpi_supported_cpu_ops; // acpi disable 상태에 따라 구조체 값이 변경됨

	while (*ops) { // operation 순회
		if (!strcmp(name, (*ops)->name)) // operation name을 copy
			return *ops; // copy 성공시, 해당 operation을 리턴

		ops++; // 다음 operation 으로
	}

	return NULL; // 적합한 operation이 없는 경우 null
}

static const char *__init cpu_read_enable_method(int cpu)
{
	/**
	 * @brief 해당 cpu의 enable_method를 가져옴
	 * @return
	 *  pass - char* (enable_method)
	 *  fail - null
	 */
	const char *enable_method;

	if (acpi_disabled) { // acpi disable인 경우
		struct device_node *dn = of_get_cpu_node(cpu, NULL); // device node를 가져옴

		if (!dn) { // 실패 케이스
			if (!cpu)
				pr_err("Failed to find device node for boot cpu\n");
			return NULL;
		}

		enable_method = of_get_property(dn, "enable-method", NULL); // enable-method를 가져옴
		if (!enable_method) { // enable-method가 없는 경우 (없는 케이스도 있음)
			/*
			 * The boot CPU may not have an enable method (e.g.
			 * when spin-table is used for secondaries).
			 * Don't warn spuriously.
			 */
			if (cpu != 0)
				pr_err("%s: missing enable-method property\n",
					dn->full_name);
		}
	} else {
		enable_method = acpi_get_enable_method(cpu); // enable-method를 가져옴
		if (!enable_method) {
			/*
			 * In ACPI systems the boot CPU does not require
			 * checking the enable method since for some
			 * boot protocol (ie parking protocol) it need not
			 * be initialized. Don't warn spuriously.
			 */
			if (cpu != 0)
				pr_err("Unsupported ACPI enable-method\n");
		}
	}

	return enable_method;
}
/*
 * Read a cpu's enable method and record it in cpu_ops.
 */
int __init cpu_read_ops(int cpu)
{
	/**
	 * @brief enable-method에 타입의 정보를 읽어온다.
	 */
	const char *enable_method = cpu_read_enable_method(cpu); // cpu id에 해당하는 operation 이름을 구한다.

	if (!enable_method) // 없는 경우
		return -ENODEV;

	cpu_ops[cpu] = cpu_get_ops(enable_method); // operation 구조체를 가져옴
	if (!cpu_ops[cpu]) { // 가져오는데 실패한 경우
		pr_warn("Unsupported enable-method: %s\n", enable_method);
		return -EOPNOTSUPP;
	}

	return 0;
}
