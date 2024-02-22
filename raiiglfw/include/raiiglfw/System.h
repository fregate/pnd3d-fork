#include <functional>

namespace raiiglfw
{

using ErrorCallback = std::function<void(int, const char *)>;

class System
{
public:
	System(ErrorCallback error_callback = nullptr);
	~System();
};

} // namespace raiiglfw
