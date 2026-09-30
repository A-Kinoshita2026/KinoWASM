#pragma once
#include <coroutine>
#include <functional>

struct IAwaiter
{
public:
    virtual ~IAwaiter() = default;

    virtual bool resume() = 0;
};

template<class Handle>
concept AwaitableHandler = requires(Handle handle)
{
    requires std::same_as<IAwaiter*, decltype(handle.promise().pAwaiter)>;
    requires std::convertible_to<Handle, std::coroutine_handle<>>;
};

/// <summary>
/// Yield
/// </summary>
struct Yield
{
    constexpr Yield() :
        count(1)
    {}

    constexpr Yield(std::uint32_t _count) :
        count(_count)
    {}

    std::uint32_t count;
};

namespace detail
{
    struct YieldAwaiter : IAwaiter
    {
        YieldAwaiter(const Yield& y):
            yield(y)
        {}
        bool await_ready() const noexcept
        {
            return yield.count == 0;
        }

        template<AwaitableHandler Handle>
        void await_suspend(Handle handle)
        {
            handle.promise().pAwaiter = this;
        }
        void await_resume() {}

        bool resume() override
        {
            return --yield.count > 0;
        }
        Yield yield;
    };
}

/// <summary>
/// operator co_await
/// </summary>
/// <param name="yield"></param>
/// <returns></returns>
inline auto operator co_await(const Yield& yield)
{
    return detail::YieldAwaiter{ yield };
}
namespace detail
{
    template<class T>
    struct PromiseType;
}

/// <summary>
/// Fiber
/// </summary>
template <class T = void>
struct Fiber
{
    using promise_type = detail::PromiseType<T>;
    using Handle = std::coroutine_handle<promise_type>;
public:
    Fiber(Handle h);

    Fiber(Fiber const&) = delete;

    Fiber(Fiber&& rhs) noexcept;

    ~Fiber();
public:
    /// <summary>
    /// 再開
    /// </summary>
    /// <returns></returns>
    bool resume() const;

    /// <summary>
    /// 完了したか
    /// </summary>
    /// <returns></returns>
    [[nodiscard]] bool isDone() const;

    /// <summary>
    /// 取得
    /// </summary>
    /// <returns></returns>
    [[nodiscard]] decltype(auto) get() const;

private:
    Handle m_coro;
};

/// <summary>
/// operator co_await
/// </summary>
/// <typeparam name="T"></typeparam>
/// <param name="other"></param>
/// <returns></returns>
template<class T>
auto operator co_await(Fiber<T> other);

/// <summary>
/// 両辺のFiberの完了を待つFiberを生成
/// </summary>
/// <typeparam name="T"></typeparam>
/// <typeparam name="U"></typeparam>
/// <param name="a"></param>
/// <param name="b"></param>
/// <returns></returns>
template<class T, class U>
[[nodiscard]] Fiber<void> operator & (Fiber<T> a, Fiber<U> b);

/// <summary>
/// 両辺のいずれかのFiberの完了を待つFiberを生成
/// </summary>
/// <typeparam name="T"></typeparam>
/// <typeparam name="U"></typeparam>
/// <param name="a"></param>
/// <param name="b"></param>
/// <returns></returns>
template<class T, class U>
[[nodiscard]] Fiber<void> operator | (Fiber<T> a, Fiber<U> b);

/// <summary>
/// 左辺のFiberの完了を待ったあと右辺のFiberを待つ
/// </summary>
/// <typeparam name="T"></typeparam>
/// <typeparam name="U"></typeparam>
/// <param name="a"></param>
/// <param name="b"></param>
/// <returns></returns>
template<class T, class U>
[[nodiscard]] Fiber<U> operator + (Fiber<T> a, Fiber<U> b);

template <class T>
inline Fiber<T>::Fiber(Handle h) :
    m_coro(h)
{}

template <class T>
inline Fiber<T>::Fiber(Fiber&& rhs) noexcept
    : m_coro(std::move(rhs.m_coro))
{
    rhs.m_coro = nullptr;
}
template <class T>
inline Fiber<T>::~Fiber()
{
    if (m_coro) {
        m_coro.destroy();
    }
}

template <class T>
inline bool Fiber<T>::resume() const
{
    if (!m_coro) {
        return false;
    }
    if (m_coro.done()) {
        return false;
    }
    // Yield
    {
        if (auto& pAwaiter = m_coro.promise().pAwaiter) {
            if (!pAwaiter->resume()) {
                pAwaiter = nullptr;
            } else {
                return true;
            }
        }
    }
    m_coro.resume();
    return !m_coro.done();
}

template <class T>
inline bool Fiber<T>::isDone() const
{
    if (!m_coro) {
        return true;
    }
    return m_coro.done();
}

template <class T>
inline decltype(auto) Fiber<T>::get() const
{
    return m_coro.promise().getValue();
}

template<class T, class U>
inline Fiber<void> operator & (Fiber<T> a, Fiber<U> b)
{
    while (true) {
        a.resume();
        b.resume();

        if (a.isDone() && b.isDone()) {
            co_return;
        }

        co_yield{};
    }
}

template<class T, class U>
inline Fiber<void> operator | (Fiber<T> a, Fiber<U> b)
{
    while (true) {
        a.resume();
        if (a.isDone()) {
            co_return;
        }
        b.resume();
        if (b.isDone()) {
            co_return;
        }

        co_yield{};
    }
}

template<class T, class U>
inline Fiber<U> operator + (Fiber<T> a, Fiber<U> b)
{
    while (a.resume()) {
        co_yield{};
    }
    while (b.resume()) {
        co_yield{};
    }
    co_return b.get();
}
namespace detail {
    template<class T>
    struct FiberAwaiter;
}

template<class T>
inline auto operator co_await(Fiber<T> other)
{
    return detail::FiberAwaiter{ std::move(other) };
}

namespace detail
{
    /// <summary>
    /// Promise
    /// </summary>
    template<class T>
    struct PromiseValue
    {
        void return_value(const T& _value)
        {
            this->value = _value;
        }

        const T& getValue() const
        {
            return value;
        }
        T value;
    };

    // void特殊化
    template<>
    struct PromiseValue<void>
    {
        void return_void() {}

        void getValue() const
        {}
    };

    template<class T>
    struct PromiseType : PromiseValue<T>
    {
        using FiberType = Fiber<T>;

        auto get_return_object() { return FiberType{ FiberType::Handle::from_promise(*this) }; }
        auto initial_suspend() { return std::suspend_always{}; }
        auto final_suspend() noexcept { return std::suspend_always{}; }
		void unhandled_exception() { std::terminate(); }

        auto yield_value(const Yield& _yield)
        {
            return operator co_await(_yield);
        }
        IAwaiter* pAwaiter = nullptr;
    };

    /// <summary>
    /// Awaiter
    /// </summary>
    template<class T>
    struct FiberAwaiter : IAwaiter
    {
        FiberAwaiter(Fiber<T>&& f):
            fiber(std::move(f))
        {}

        bool await_ready() const
        {
            return !fiber.resume();
        }
        template<AwaitableHandler Handle>
        void await_suspend(Handle handle)
        {
            handle.promise().pAwaiter = this;
        }
        decltype(auto) await_resume() const
        {
            return fiber.get();
        }
        bool resume() override
        {
            return fiber.resume();
        }

        Fiber<T> fiber;
    };
}
