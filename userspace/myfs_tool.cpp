#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "myfs_ioctl.h"

namespace {

namespace fs = std::filesystem;

const char *const kDefaultMount = "/mnt";

std::system_error sys_error(const std::string &what)
{
	return std::system_error(errno, std::generic_category(), what);
}

class Fd {
public:
	Fd(const std::string &path, int flags) : fd_(::open(path.c_str(), flags))
	{
		if (fd_ < 0)
			throw sys_error("open " + path);
	}
	~Fd() { ::close(fd_); }
	Fd(const Fd &) = delete;
	Fd &operator=(const Fd &) = delete;
	int get() const { return fd_; }

private:
	int fd_;
};

void myfs_ioctl(const std::string &mnt, unsigned long cmd, void *arg, const char *name)
{
	Fd dir(mnt, O_RDONLY | O_DIRECTORY);
	if (::ioctl(dir.get(), cmd, arg) < 0)
		throw sys_error(name);
}

std::vector<fs::path> list_files(const std::string &mnt)
{
	std::vector<fs::path> files;
	for (const auto &entry : fs::directory_iterator(mnt))
		if (entry.is_regular_file())
			files.push_back(entry.path());
	std::sort(files.begin(), files.end());
	return files;
}

int cmd_test(const std::string &mnt)
{
	auto files = list_files(mnt);
	if (files.empty()) {
		std::cerr << "no files in " << mnt << "\n";
		return 1;
	}

	std::mt19937_64 rng(std::random_device{}());
	std::vector<std::string> written;
	size_t errors = 0;

	for (const auto &path : files) {
		std::string text = std::to_string(rng()) + "\n";
		Fd fd(path, O_WRONLY);
		ssize_t n = ::pwrite(fd.get(), text.data(), text.size(), 0);
		if (n != (ssize_t)text.size()) {
			std::cerr << path.filename().string() << ": write: "
				  << (n < 0 ? std::strerror(errno) : "short write") << "\n";
			++errors;
		}
		written.push_back(text);
	}

	for (size_t i = 0; i < files.size(); ++i) {
		std::string text(written[i].size(), '\0');
		Fd fd(files[i], O_RDONLY);
		ssize_t n = ::pread(fd.get(), text.data(), text.size(), 0);
		if (n != (ssize_t)text.size() || text != written[i]) {
			std::cerr << files[i].filename().string() << ": read back "
				  << (n < 0 ? std::strerror(errno) : "different data") << "\n";
			++errors;
		}
	}

	std::cout << files.size() << " files written and read back, " << errors << " errors\n";
	return errors ? 1 : 0;
}

int cmd_zero(const std::string &mnt)
{
	myfs_ioctl(mnt, MYFS_IOC_ZERO_ALL, nullptr, "MYFS_IOC_ZERO_ALL");
	std::cout << "all files zeroed\n";
	return 0;
}

int cmd_erase(const std::string &mnt, bool confirmed)
{
	if (!confirmed) {
		std::cout << "Erase the filesystem mounted at " << mnt << "? Type yes: ";
		std::string answer;
		std::cin >> answer;
		if (answer != "yes") {
			std::cout << "aborted\n";
			return 1;
		}
	}
	myfs_ioctl(mnt, MYFS_IOC_ERASE_FS, nullptr, "MYFS_IOC_ERASE_FS");
	std::cout << "filesystem erased; it cannot be mounted again until recreated with format=1\n";
	return 0;
}

int cmd_metadata(const std::string &mnt)
{
	myfs_meta_req req{};
	myfs_ioctl(mnt, MYFS_IOC_GET_META, &req, "MYFS_IOC_GET_META");

	std::vector<myfs_file_meta> meta(req.count);
	req.entries = reinterpret_cast<uintptr_t>(meta.data());
	myfs_ioctl(mnt, MYFS_IOC_GET_META, &req, "MYFS_IOC_GET_META");
	meta.resize(std::min<size_t>(meta.size(), req.count));

	std::cout << "index  name        crc32\n";
	for (const auto &m : meta)
		std::cout << std::setw(5) << m.index << "  " << std::left << std::setw(10) << m.name
			  << std::right << "  0x" << std::hex << std::setw(8) << std::setfill('0')
			  << m.crc32 << std::dec << std::setfill(' ') << "\n";
	return 0;
}

int cmd_sectormap(const std::string &name, const std::string &mnt)
{
	myfs_sector_map map{};
	if (name.size() >= sizeof(map.name)) {
		std::cerr << "file name too long: " << name << "\n";
		return 1;
	}
	std::memcpy(map.name, name.c_str(), name.size() + 1);
	myfs_ioctl(mnt, MYFS_IOC_GET_SECTOR_MAP, &map, "MYFS_IOC_GET_SECTOR_MAP");

	uint64_t first_byte = uint64_t(map.start) * map.sector_size;
	uint64_t last_byte = first_byte + uint64_t(map.count) * map.sector_size - 1;
	std::cout << name << ": index " << map.index << ", " << map.count << " sectors of "
		  << map.sector_size << " bytes\n"
		  << "  sectors " << map.start << "-" << map.start + map.count - 1
		  << "  (bytes " << first_byte << "-" << last_byte << " of the device)\n";
	return 0;
}

void usage(const char *prog)
{
	std::cerr << "usage: " << prog << " test [mnt]\n"
		  << "       " << prog << " zero [mnt]\n"
		  << "       " << prog << " erase [-y] [mnt]\n"
		  << "       " << prog << " metadata [mnt]\n"
		  << "       " << prog << " sectormap <file> [mnt]\n"
		  << "mnt defaults to " << kDefaultMount << "\n";
}

}

int main(int argc, char *argv[])
{
	std::vector<std::string> args(argv + 1, argv + argc);
	if (args.empty()) {
		usage(argv[0]);
		return 2;
	}
	const std::string cmd = args[0];
	args.erase(args.begin());

	bool confirmed = false;
	if (cmd == "erase" && !args.empty() && args[0] == "-y") {
		confirmed = true;
		args.erase(args.begin());
	}

	std::string file;
	if (cmd == "sectormap") {
		if (args.empty()) {
			usage(argv[0]);
			return 2;
		}
		file = args[0];
		args.erase(args.begin());
	}
	if (args.size() > 1) {
		usage(argv[0]);
		return 2;
	}
	const std::string mnt = args.empty() ? kDefaultMount : args[0];

	try {
		if (cmd == "test")
			return cmd_test(mnt);
		if (cmd == "zero")
			return cmd_zero(mnt);
		if (cmd == "erase")
			return cmd_erase(mnt, confirmed);
		if (cmd == "metadata")
			return cmd_metadata(mnt);
		if (cmd == "sectormap")
			return cmd_sectormap(file, mnt);
	} catch (const std::exception &e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
	usage(argv[0]);
	return 2;
}
