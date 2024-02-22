#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace raiiglfw
{

using WindowHints = std::map<int, int>;
using KeyCallback = std::function<void(int, int, int, int)>;

class IODevice
{
public:
	virtual ~IODevice() = default;

 	virtual void init() = 0;
	virtual void set_key_callback(KeyCallback callback) = 0;

	virtual void run() = 0;
};

using IODevicePtr = std::unique_ptr<IODevice>;
IODevicePtr make_io_device(std::int32_t width, std::int32_t height, std::string title, WindowHints hints = {});

} // namespace raiiglfw
