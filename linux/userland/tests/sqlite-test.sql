-- sqlite-test.sql: a known-answer test of the KlaussCPU sqlite3 build.
-- sqlite-test.sh runs it against a database file and compares the output
-- with sqlite-test.expected (made on the PC from the same amalgamation).
-- Floats from libm are rounded, since musl and glibc may differ in the
-- last bit; SQLite formats doubles itself.
.headers off
.mode list
.separator |
PRAGMA journal_mode=WAL;
PRAGMA foreign_keys=ON;
DROP TABLE IF EXISTS item; DROP TABLE IF EXISTS cat; DROP TABLE IF EXISTS log;
DROP VIEW IF EXISTS cat_totals;

CREATE TABLE cat(id INTEGER PRIMARY KEY, name TEXT NOT NULL COLLATE NOCASE UNIQUE);
CREATE TABLE item(id INTEGER PRIMARY KEY, cat INTEGER REFERENCES cat(id),
                  qty INTEGER, price REAL, note TEXT);
CREATE TABLE log(id INTEGER PRIMARY KEY, what TEXT);
CREATE INDEX item_cat ON item(cat, price);
CREATE TRIGGER item_ins AFTER INSERT ON item WHEN new.qty > 990
  BEGIN INSERT INTO log(what) VALUES ('big ' || new.id); END;

INSERT INTO cat(name) VALUES ('alpha'), ('Beta'), ('gamma'), ('Delta'), ('epsilon');
-- 5000 rows from a recursive CTE: deterministic pseudo-random values.
WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 5000)
INSERT INTO item(cat, qty, price, note)
  SELECT 1 + (i * 7919) % 5, (i * 104729) % 1000, ((i * 7907) % 10007) / 100.0,
         printf('n%04d-%s', i, substr('abcdefghij', 1 + i % 10, 3))
  FROM n;

SELECT 'count', count(*), sum(qty), min(price), max(price) FROM item;
SELECT 'avg', round(avg(price), 6), round(total(price * qty), 2) FROM item;
SELECT 'log', count(*), min(what), max(what) FROM log;
SELECT 'group', c.name, count(*), sum(i.qty), round(avg(i.price), 4)
  FROM item i JOIN cat c ON c.id = i.cat GROUP BY c.name ORDER BY c.name;
SELECT 'having', cat, count(*) FROM item GROUP BY cat HAVING sum(qty) > 499000 ORDER BY cat;
SELECT 'nocase', id FROM cat WHERE name = 'BETA';
SELECT 'like', count(*) FROM item WHERE note LIKE '%-cde';
SELECT 'glob', count(*) FROM item WHERE note GLOB 'n1*[ae]?[cg]';

-- Subqueries and windows
SELECT 'top', id, price FROM item WHERE price = (SELECT max(price) FROM item) ORDER BY id LIMIT 3;
SELECT 'rank', id, qty, rank() OVER (ORDER BY qty DESC, id) FROM item ORDER BY qty DESC, id LIMIT 4;
SELECT 'running', id, sum(qty) OVER (ORDER BY id ROWS BETWEEN 2 PRECEDING AND CURRENT ROW)
  FROM item WHERE id <= 5 ORDER BY id;
SELECT 'ntile', ntile(4) OVER (ORDER BY price), id FROM item WHERE id % 1000 = 0 ORDER BY price;
SELECT 'lag', id, qty - lag(qty, 1, 0) OVER (ORDER BY id) FROM item WHERE id <= 4;
SELECT 'exists', count(*) FROM cat c WHERE EXISTS (SELECT 1 FROM item WHERE cat = c.id AND qty = 999);

-- Views and a self-join
CREATE VIEW cat_totals AS SELECT cat, sum(qty) AS q, count(*) AS n FROM item GROUP BY cat;
SELECT 'view', a.cat, b.cat, a.q - b.q FROM cat_totals a JOIN cat_totals b ON b.cat = a.cat + 1 ORDER BY a.cat;

-- Strings and Unicode
SELECT 'str', upper('KlaussCPU'), lower('ÄBC'), length('añb€'), length(CAST('añb€' AS BLOB)),
       substr('hello world', -5), replace('a-b-c', '-', '+'), instr('banana', 'nan'),
       trim('  x  '), hex('ab'), unicode('€'), char(72, 105);
SELECT 'printf', printf('%5.2f|%-5d|%x|%q|%e', 3.14159, 42, 255, 'it''s', 12345.678);
SELECT 'concat', 'a' || 1 || 2.5 || NULL IS NULL, coalesce(NULL, 'b'), ifnull(NULL, 3), nullif(4, 4);

-- Numbers
SELECT 'int', 9223372036854775807 + 0, -9223372036854775808, 7 / 2, -7 / 2, 7 % -3, 1 << 62, -1 >> 60;
SELECT 'ovf', typeof(9223372036854775807 + 1), 9223372036854775807 + 1.0 > 9.2e18;
SELECT 'real', 0.1 + 0.2, 1.0 / 3, 2.5e-10 * 4, CAST(7.9 AS INTEGER), CAST('12abc' AS INTEGER), 10 / 4.0;
SELECT 'round', round(2.5), round(-2.5), round(1.23456789, 4), round(1e300 * 1e10 > 0);
SELECT 'math', round(sqrt(2), 9), round(exp(1), 9), round(ln(10), 9), round(sin(1), 9),
       round(pow(2, 0.5), 9), round(atan2(1, 1) * 4, 9), floor(-2.5), ceil(2.1), mod(10.5, 3);
SELECT 'series', round(sum(1.0 / (value * value)), 10)
  FROM (WITH RECURSIVE s(value) AS (SELECT 1 UNION ALL SELECT value + 1 FROM s WHERE value < 10000) SELECT value FROM s);
SELECT 'checksum', sum((qty * 31 + id) % 997), sum(CAST(price * 100 AS INTEGER) % 101) FROM item;

-- JSON and dates
SELECT 'json', json_extract('{"a":[1,2,{"b":"x"}]}', '$.a[2].b'), json_array_length('[1,2,3]'),
       json_object('k', 1, 'v', json('[true,null]'));
SELECT 'jgroup', json_group_array(id) FROM (SELECT id FROM cat ORDER BY id DESC);
SELECT 'date', date('2026-11-10', '+1 month', 'start of month'), strftime('%j|%w', '2026-11-10'),
       julianday('2026-11-10') - julianday('2026-01-01'), datetime(1793664000, 'unixepoch');

-- Transactions and savepoints
BEGIN;
DELETE FROM item WHERE id > 4000;
SELECT 'in-txn', count(*) FROM item;
ROLLBACK;
SELECT 'rolled-back', count(*) FROM item;
BEGIN;
UPDATE item SET qty = qty + 1 WHERE cat = 3;
SAVEPOINT sp1;
DELETE FROM item WHERE cat = 3;
SELECT 'sp-inner', count(*) FROM item WHERE cat = 3;
ROLLBACK TO sp1;
RELEASE sp1;
COMMIT;
SELECT 'committed', count(*), sum(qty) FROM item WHERE cat = 3;
INSERT INTO item(cat, qty, price, note) VALUES (9, 1, 1.0, 'bad fk');
SELECT 'fk', count(*) FROM item WHERE cat = 9;
UPDATE item SET note = NULL WHERE id % 2 = 0;
SELECT 'nulls', count(note), count(*) - count(note) FROM item;
PRAGMA wal_checkpoint(TRUNCATE);
PRAGMA integrity_check;
VACUUM;
SELECT 'pages', page_count > 0 FROM pragma_page_count();
