"""
示例步骤实现：对应 specs/login.spec、specs/calculator.spec、specs/checkout.spec。
这些实现只操作内存状态，用于演示 Runner 协议与数据表驱动。
"""

from testhub_runner import (
    DataTable,
    Messages,
    after_scenario,
    before_scenario,
    data_store,
    step,
)

VALID_USERS = {"admin": "secret123", "alice": "password", "bob": "password", "carol": "password"}


@before_scenario
def reset_state():
    data_store.scenario.update({"page": None, "username": "", "password": "", "message": "", "cart": [], "order": None})


@after_scenario
def report_state():
    Messages.write(f"scenario finished on page={data_store.scenario.get('page')}")


# ---------------- 登录 ----------------

@step("打开登录页面")
def open_login_page():
    data_store.scenario["page"] = "login"


@step("输入用户名 <name>")
def enter_username(name):
    data_store.scenario["username"] = name


@step("输入密码 <password>")
def enter_password(password):
    data_store.scenario["password"] = password


@step("点击登录按钮")
def click_login():
    s = data_store.scenario
    if not s["username"]:
        s["message"] = "请输入用户名"
    elif VALID_USERS.get(s["username"]) == s["password"]:
        s["message"] = f"欢迎, {s['username']}"
        s["page"] = "home"
    else:
        s["message"] = "用户名或密码错误"
    Messages.write(f"login attempt for {s['username']!r} -> {s['message']}")


@step("应该看到欢迎信息 <text>")
def should_see_welcome(text):
    actual = data_store.scenario["message"]
    # 概念 auth.cpt 传入的是用户名，spec 中传入的是完整欢迎语，两者都接受
    assert actual == text or actual == f"欢迎, {text}", f"expected welcome {text!r}, got {actual!r}"


@step("应该看到错误提示 <text>")
def should_see_error(text):
    actual = data_store.scenario["message"]
    assert actual == text, f"expected error {text!r}, got {actual!r}"


@step("清理浏览器会话")
def clear_session():
    data_store.scenario["page"] = None


# ---------------- 计算器 ----------------

@step("输入第一个数 <a>")
def first_number(a):
    data_store.scenario["a"] = float(a)


@step("输入第二个数 <b>")
def second_number(b):
    data_store.scenario["b"] = float(b)


@step("点击加号")
def press_plus():
    data_store.scenario["result"] = data_store.scenario.get("a", 0) + data_store.scenario.get("b", 0)


@step("点击清空")
def press_clear():
    data_store.scenario["result"] = 0


@step("结果应该是 <expected>")
def result_should_be(expected):
    actual = data_store.scenario.get("result")
    assert actual == float(expected), f"expected {expected}, got {actual}"


# ---------------- 购物车 ----------------

@step("将商品 <item> 加入购物车")
def add_item(item):
    data_store.scenario["cart"].append((item, 1))


@step("批量加入以下商品 <table>")
def add_items(table: DataTable):
    for row in table:
        data_store.scenario["cart"].append((row["商品"], int(row["数量"])))
    Messages.write(f"added {len(table)} line(s)")


@step("购物车中应该有 <count> 件商品")
def cart_should_have(count):
    total = sum(q for _, q in data_store.scenario["cart"])
    assert total == int(count), f"expected {count} item(s), cart has {total}"


@step("结算并使用 <method> 支付")
def checkout(method):
    data_store.scenario["order"] = "支付失败" if "余额不足" in method else "已支付"


@step("订单状态应该是 <status>")
def order_status(status):
    actual = data_store.scenario["order"]
    assert actual == status, f"expected order status {status!r}, got {actual!r}"


# ---------------- 通用 ----------------

@step("等待 <seconds> 秒")
def wait_seconds(seconds):
    """用于演示实时监控、超时与取消；真实项目中通常不需要显式等待。"""
    import time

    delay = float(seconds)
    Messages.write(f"sleeping {delay:g}s")
    time.sleep(delay)
