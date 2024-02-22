#include <exception>
#include <system_error>

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

#include <raiiglfw/System.h>

namespace raiiglfw
{

namespace
{

// Hack to run std::function as C-like callback
auto get_callback() -> ErrorCallback &
{
	static ErrorCallback callback;
	return callback;
};

auto set_callback(ErrorCallback && func) -> void
{
	auto & callback = get_callback();
	callback = std::move(func);
}

void error_callback_global(int code, const char * description)
{
	get_callback()(code, description);
}

} // namespace

System::System(ErrorCallback error_callback)
{
	spdlog::info("Initializing GLFW...");

	if (!glfwInit())
	{
		throw std::system_error(std::make_error_code(std::errc::no_such_device), "Can't init GLFW!");
	}

	if (error_callback)
	{
		spdlog::info("Set error callback");
		set_callback(std::move(error_callback));
		glfwSetErrorCallback(error_callback_global);
	}

	spdlog::info("Initializing done");
}

System::~System()
{
	spdlog::info("Terminating GLFW...");
	glfwTerminate();
	spdlog::info("Terminating done");
}

} // namespace raiiglfw
