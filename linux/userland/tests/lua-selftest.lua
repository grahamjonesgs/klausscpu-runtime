-- lua-selftest.lua: known-answer checks of the KlaussCPU Lua build
-- (strings, tables, closures, coroutines, integers and floats, luac).
-- Prints one line per group and "lua-selftest: PASS" or "... FAIL".
local fails = 0
local function check(group, cond, what)
  if not cond then
    fails = fails + 1
    print(("lua-selftest: %s: FAIL %s"):format(group, what))
  end
end

-- Strings
do
  local s = ("KlaussCPU"):rep(3, "-")
  check("strings", s == "KlaussCPU-KlaussCPU-KlaussCPU", "rep")
  check("strings", s:upper():sub(-3) == "CPU" and #s == 29, "upper/sub/len")
  check("strings", ("%5.2f|%-4d|%x|%q"):format(3.14159, 42, 255, "a\n") == ' 3.14|42  |ff|"a\\\n"', "format")
  local words = {}
  for w in ("the quick brown fox"):gmatch("%a+") do words[#words + 1] = w end
  check("strings", table.concat(words, ",") == "the,quick,brown,fox", "gmatch")
  check("strings", ("hello world"):gsub("o", "0") == "hell0 w0rld", "gsub")
  check("strings", string.pack(">I4", 0x01020304) == "\1\2\3\4", "pack")
  check("strings", utf8.len("añb€") == 4 and utf8.char(8364) == "€", "utf8")
  print("lua-selftest: strings")
end

-- Tables and sorting
do
  local t = {}
  local lo, hi = math.huge, -math.huge
  for i = 1, 1000 do
    t[i] = (i * 7919) % 1009
    lo, hi = math.min(lo, t[i]), math.max(hi, t[i])
  end
  table.sort(t)
  local ok = true
  for i = 2, #t do ok = ok and t[i - 1] < t[i] end	-- 1009 is prime: all distinct
  check("tables", ok and t[1] == lo and t[1000] == hi, "sort")
  local h = {}
  for i = 1, 500 do h["k" .. i] = i * i end
  local sum = 0
  for _, v in pairs(h) do sum = sum + v end
  check("tables", sum == 41791750, "hash sum " .. sum)
  check("tables", select("#", table.unpack({1, 2, nil, 4}, 1, 4)) == 4, "unpack")
  table.insert(t, 1, -1); table.remove(t)
  check("tables", t[1] == -1 and #t == 1000, "insert/remove")
  print("lua-selftest: tables")
end

-- Closures and recursion
do
  local function counter()
    local n = 0
    return function() n = n + 1; return n end
  end
  local a, b = counter(), counter()
  a(); a(); b()
  check("closures", a() == 3 and b() == 2, "upvalues")
  local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end
  check("closures", fib(20) == 6765, "fib")
  local adders = {}
  for i = 1, 5 do adders[i] = function(x) return x + i end end
  check("closures", adders[3](10) == 13 and adders[5](0) == 5, "loop upvalues")
  print("lua-selftest: closures")
end

-- Coroutines
do
  local gen = coroutine.wrap(function()
    for i = 1, 5 do coroutine.yield(i * i) end
    return "done"
  end)
  local got = {}
  for _ = 1, 6 do got[#got + 1] = tostring(gen()) end
  check("coroutines", table.concat(got, " ") == "1 4 9 16 25 done", "wrap")
  local co = coroutine.create(function(x, y)
    local z = coroutine.yield(x + y)
    error("boom " .. z, 0)
  end)
  local ok1, v1 = coroutine.resume(co, 2, 3)
  local ok2, v2 = coroutine.resume(co, 7)
  check("coroutines", ok1 and v1 == 5 and not ok2 and v2 == "boom 7", "resume/error")
  check("coroutines", coroutine.status(co) == "dead", "status")
  print("lua-selftest: coroutines")
end

-- Integers and floats
do
  check("numbers", math.maxinteger + 1 == math.mininteger, "integer wrap")
  check("numbers", 7 // 2 == 3 and -7 // 2 == -4 and 7 % -3 == -2, "floor div/mod")
  check("numbers", 1 << 62 == 4611686018427387904 and (-1) >> 60 == 15, "shifts")
  check("numbers", math.type(1) == "integer" and math.type(1.0) == "float", "types")
  check("numbers", 0.1 + 0.2 ~= 0.3 and math.abs(0.1 + 0.2 - 0.3) < 1e-15, "fp rounding")
  check("numbers", ("%.17g"):format(1 / 3) == "0.33333333333333331", "1/3")
  check("numbers", ("%.6f"):format(math.sqrt(2)) == "1.414214" and math.floor(-2.5) == -3, "sqrt/floor")
  check("numbers", ("%.10g"):format(math.sin(1) ^ 2 + math.cos(1) ^ 2) == "1", "sin/cos")
  check("numbers", ("%.12g"):format(math.exp(math.log(10))) == "10", "exp/log")
  check("numbers", tostring(1e300 * 1e10) == "inf" and (0/0) ~= (0/0), "inf/nan")
  check("numbers", math.tointeger(3.0) == 3 and tonumber("0x1p4") == 16.0, "conversions")
  local acc = 0.0
  for i = 1, 10000 do acc = acc + 1 / (i * i) end
  check("numbers", ("%.10f"):format(acc) == "1.6448340718", "series " .. ("%.10f"):format(acc))
  print("lua-selftest: numbers")
end

-- luac: compile a chunk to bytecode and load it back
do
  local f = string.dump(load("local a, b = ... return a * b + 1"))
  check("bytecode", load(f)(6, 7) == 43, "dump/load")
  print("lua-selftest: bytecode")
end

print(fails == 0 and "lua-selftest: PASS" or ("lua-selftest: FAIL (%d)"):format(fails))
