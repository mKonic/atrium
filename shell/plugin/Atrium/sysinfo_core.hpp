#pragma once
// Reading what the About card shows, as plain C++ so it is tested without Qt.

#include <istream>
#include <string>
#include <string_view>

namespace atrium::sysinfo {

// A key of /etc/os-release (quotes removed), or "".
std::string os_release_value(std::string_view text, std::string_view key);

// "Vendor Device" for PCI ids from a pci.ids database, shortened to the
// bracketed marketing name when there is one ("GB206 [GeForce RTX 5060 Ti]"
// → "GeForce RTX 5060 Ti"). "" when unknown.
std::string pci_device_name(std::istream& ids, unsigned vendor, unsigned device);

// "AMD Ryzen 7 7800X3D 8-Core Processor" → "AMD Ryzen 7 7800X3D" style
// tidying of /proc/cpuinfo's model name.
std::string tidy_cpu_name(std::string_view model);

// Installed memory from udev's record of the SMBIOS memory devices
// (/run/udev/data/+dmi:id, readable without root): "64 GB DDR5-6000".
// "" when udev has none.
std::string installed_memory(std::string_view udev_dmi);

// The size of a PCI device's largest memory window, from its sysfs
// `resource` file: a discrete GPU maps its VRAM, integrated graphics little.
unsigned long long largest_bar(std::string_view resource);

} // namespace atrium::sysinfo
