#include <exception>
#include <system_error>

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

#include <raiiglfw/IODevice.h>

namespace raiiglfw
{

namespace
{

// Hack to run std::function as C-like callback
auto get_callback() -> KeyCallback &
{
	static KeyCallback callback;
	return callback;
};

auto set_callback(KeyCallback && func) -> void
{
	auto & callback = get_callback();
	callback = std::move(func);
}

void key_callback_global(GLFWwindow *, int key, int scancode, int action, int mods)
{
	get_callback()(key, scancode, action, mods);
}

} // namespace

class IODevicePrivate : public IODevice
{
public:
	IODevicePrivate(std::int32_t width, std::int32_t height, std::string title, WindowHints hints);
	~IODevicePrivate() override;

	void init() override;
	void set_key_callback(KeyCallback callback) override;

	void run() override;

private:
	GLFWwindow * window_;
};

IODevicePrivate::IODevicePrivate(std::int32_t width, std::int32_t height, std::string title, WindowHints hints)
{
	spdlog::info("Creating window...");
	if (!hints.empty())
	{
		for (const auto [k, v] : hints)
		{
			glfwWindowHint(k, v);
		}
	}

	window_ = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
	if (window_ == nullptr)
	{
		throw std::system_error(std::make_error_code(std::errc::broken_pipe), "Can't create window");
	}
	spdlog::info("Window created");
}

IODevicePrivate::~IODevicePrivate()
{
	glfwDestroyWindow(window_);
	spdlog::info("Window destroyed");
}

void IODevicePrivate::set_key_callback(KeyCallback callback)
{
	if (!callback)
		return;

	set_callback(std::move(callback));
	glfwSetKeyCallback(window_, key_callback_global);
	spdlog::info("Set keyboard callback");
}

void IODevicePrivate::init()
{
	glfwMakeContextCurrent(window_);
	// gladLoadGL(glfwGetProcAddress);
	glfwSwapInterval(1);
	spdlog::info("IODevice inited");
}

void IODevicePrivate::run()
{
	spdlog::info("IODevice running...");
	int width, height;
	while (!glfwWindowShouldClose(window_))
	{
		glClear(GL_COLOR_BUFFER_BIT);

		glfwSwapBuffers(window_);
		glfwPollEvents();
	}
	spdlog::info("IODevice stopped");
}

IODevicePtr make_io_device(std::int32_t width, std::int32_t height, std::string title, WindowHints hints)
{
	return std::make_unique<IODevicePrivate>(width, height, std::move(title), std::move(hints));
}

} // namespace raiiglfw
