import argparse
import datetime
import decimal
import os
import random
import re
import sqlite3
import subprocess
import sys

TABLES = {
    "region": [("r_regionkey", "int"), ("r_name", "text"), ("r_comment", "text")],
    "nation": [("n_nationkey", "int"), ("n_name", "text"), ("n_regionkey", "int"),
               ("n_comment", "text")],
    "supplier": [("s_suppkey", "int"), ("s_name", "text"), ("s_address", "text"),
                 ("s_nationkey", "int"), ("s_phone", "text"), ("s_acctbal", "real"),
                 ("s_comment", "text")],
    "part": [("p_partkey", "int"), ("p_name", "text"), ("p_mfgr", "text"), ("p_brand", "text"),
             ("p_type", "text"), ("p_size", "int"), ("p_container", "text"),
             ("p_retailprice", "real"), ("p_comment", "text")],
    "partsupp": [("ps_partkey", "int"), ("ps_suppkey", "int"), ("ps_availqty", "int"),
                 ("ps_supplycost", "real"), ("ps_comment", "text")],
    "customer": [("c_custkey", "int"), ("c_name", "text"), ("c_address", "text"),
                 ("c_nationkey", "int"), ("c_phone", "text"), ("c_acctbal", "real"),
                 ("c_mktsegment", "text"), ("c_comment", "text")],
    "orders": [("o_orderkey", "int"), ("o_custkey", "int"), ("o_orderstatus", "text"),
               ("o_totalprice", "real"), ("o_orderdate", "date"), ("o_orderpriority", "text"),
               ("o_clerk", "text"), ("o_shippriority", "int"), ("o_comment", "text")],
    "lineitem": [("l_orderkey", "int"), ("l_partkey", "int"), ("l_suppkey", "int"),
                 ("l_linenumber", "int"), ("l_quantity", "real"), ("l_extendedprice", "real"),
                 ("l_discount", "real"), ("l_tax", "real"), ("l_returnflag", "text"),
                 ("l_linestatus", "text"), ("l_shipdate", "date"), ("l_commitdate", "date"),
                 ("l_receiptdate", "date"), ("l_shipinstruct", "text"), ("l_shipmode", "text"),
                 ("l_comment", "text")],
}

JOINS = [
    ("lineitem", "orders", "l_orderkey", "o_orderkey"),
    ("orders", "customer", "o_custkey", "c_custkey"),
    ("lineitem", "supplier", "l_suppkey", "s_suppkey"),
    ("customer", "nation", "c_nationkey", "n_nationkey"),
    ("supplier", "nation", "s_nationkey", "n_nationkey"),
    ("nation", "region", "n_regionkey", "r_regionkey"),
    ("partsupp", "part", "ps_partkey", "p_partkey"),
]


DATE_CALL = re.compile(
    r"date\s+'(\d{4}-\d{2}-\d{2})'(?:\s*([+-])\s*interval\s+'(\d+)'\s+(day|days|month|months|year|years))?")


def shift_date(text, sign, amount, unit):
    start = datetime.date.fromisoformat(text)
    step = int(amount) * (-1 if sign == "-" else 1)
    if unit.startswith("day"):
        return (start + datetime.timedelta(days=step)).isoformat()
    months = step * 12 if unit.startswith("year") else step
    total = start.year * 12 + start.month - 1 + months
    return datetime.date(total // 12, total % 12 + 1, start.day).isoformat()


DECIMAL_MATH = re.compile(r"(\d+\.\d+)\s*([+-])\s*(\d+\.\d+)")


def fold_decimals(sql):
    def replace(match):
        left = decimal.Decimal(match.group(1))
        right = decimal.Decimal(match.group(3))
        total = left + right if match.group(2) == "+" else left - right
        places = max(-left.as_tuple().exponent, -right.as_tuple().exponent)
        return ("%." + str(places) + "f") % total

    return DECIMAL_MATH.sub(replace, sql)


def to_sqlite(sql):
    def replace(match):
        if match.group(2) is None:
            return "'%s'" % match.group(1)
        return "'%s'" % shift_date(match.group(1), match.group(2), match.group(3), match.group(4))

    return fold_decimals(DATE_CALL.sub(replace, sql))


def build_sqlite(directory):
    connection = sqlite3.connect(":memory:")
    for table, columns in TABLES.items():
        types = ", ".join("%s %s" % (name, kind) for name, kind in columns)
        connection.execute("create table %s (%s)" % (table, types))
        rows = []
        path = os.path.join(directory, table + ".tbl")
        with open(path, "r", encoding="utf-8") as handle:
            for line in handle:
                line = line.rstrip("\n")
                if not line:
                    continue
                fields = line.split("|")[: len(columns)]
                values = []
                for (name, kind), field in zip(columns, fields):
                    if kind == "int":
                        values.append(int(field))
                    elif kind == "real":
                        values.append(float(field))
                    else:
                        values.append(field)
                rows.append(values)
        marks = ", ".join("?" for _ in columns)
        connection.executemany("insert into %s values (%s)" % (table, marks), rows)
    connection.commit()
    return connection


def sample_values(connection, table, column, count):
    cursor = connection.execute(
        "select %s from %s order by %s limit %d" % (column, table, column, count * 4))
    values = [row[0] for row in cursor.fetchall()]
    random.shuffle(values)
    return values[:count] if values else [0]


def literal(value):
    if isinstance(value, str):
        return "'" + value.replace("'", "") + "'"
    if isinstance(value, float):
        return "%.2f" % value
    return str(value)


def random_predicate(connection, table, columns):
    name, kind = random.choice(columns)
    values = sample_values(connection, table, name, 4)
    value = random.choice(values)
    if kind == "date":
        operator = random.choice(["=", "<", "<=", ">", ">="])
        return "%s %s %s" % (name, operator, literal(value))
    if kind == "text" and random.random() < 0.3:
        text = str(value)
        piece = text[: max(1, len(text) // 3)]
        return "%s like '%s%%'" % (name, piece.replace("'", ""))
    if random.random() < 0.2:
        low, high = sorted(random.sample(values, 2)) if len(values) > 1 else (value, value)
        return "%s between %s and %s" % (name, literal(low), literal(high))
    if random.random() < 0.2 and len(values) > 1:
        return "%s in (%s)" % (name, ", ".join(literal(v) for v in values[:3]))
    operator = random.choice(["=", "<>", "<", "<=", ">", ">="])
    return "%s %s %s" % (name, operator, literal(value))


def random_query(connection):
    join = random.random() < 0.45
    if join:
        left, right, left_key, right_key = random.choice(JOINS)
        tables = [left, right]
        columns = TABLES[left] + TABLES[right]
        source = "%s, %s" % (left, right)
        conditions = ["%s = %s" % (left_key, right_key)]
    else:
        table = random.choice(list(TABLES))
        tables = [table]
        columns = TABLES[table]
        source = table
        conditions = []
    for _ in range(random.randint(1, 3)):
        chosen = random.choice(tables)
        conditions.append(random_predicate(connection, chosen, TABLES[chosen]))
    numeric = [name for name, kind in columns if kind in ("int", "real")]
    groupable = [name for name, kind in columns if kind in ("text", "date")] + numeric
    shape = random.random()
    if shape < 0.45:
        key = random.choice(groupable)
        value = random.choice(numeric)
        function = random.choice(["sum", "count", "min", "max", "avg"])
        select = "%s, %s(%s) as measure, count(*) as lines" % (key, function, value)
        tail = " group by %s order by %s" % (key, key)
    elif shape < 0.7:
        value = random.choice(numeric)
        other = random.choice(numeric)
        select = ("sum(%s) as total, count(*) as lines, min(%s) as smallest, max(%s) as largest"
                  % (value, other, other))
        tail = ""
    else:
        picked = random.sample([name for name, _ in columns], min(3, len(columns)))
        select = ", ".join(picked)
        tail = " order by %s limit 20" % ", ".join(picked)
    where = " where " + " and ".join(conditions) if conditions else ""
    return "select %s from %s%s%s" % (select, source, where, tail)


def run_vcqe(binary, directory, sql, mode):
    result = subprocess.run(
        [binary, "query", "--dir", directory, "--sql", sql, "--print", "-1", "--quiet",
         "--mode", mode],
        capture_output=True, text=True)
    if result.returncode != 0:
        return None, result.stderr.strip()
    lines = result.stdout.splitlines()
    return [line.split("|") for line in lines[1:]], None


def normalize(value):
    if value is None:
        return "NULL"
    if isinstance(value, float):
        return round(value, 4)
    if isinstance(value, int):
        return float(value)
    return str(value)


def same_cell(left, right):
    if isinstance(right, (int, float)) and not isinstance(right, bool):
        try:
            return abs(float(left) - float(right)) <= 1e-4 * max(1.0, abs(float(right)))
        except ValueError:
            return False
    return str(left) == str(right)


def compare(vcqe_rows, sqlite_rows, ordered):
    if len(vcqe_rows) != len(sqlite_rows):
        return "row count %d against %d" % (len(vcqe_rows), len(sqlite_rows))
    if not ordered:
        vcqe_rows = sorted(vcqe_rows, key=lambda row: [str(cell) for cell in row])
        sqlite_rows = sorted(sqlite_rows, key=lambda row: [str(normalize(cell)) for cell in row])
    for index, (left, right) in enumerate(zip(vcqe_rows, sqlite_rows)):
        if len(left) != len(right):
            return "row %d has %d columns against %d" % (index, len(left), len(right))
        for column, (cell, expected) in enumerate(zip(left, right)):
            if expected is None:
                if cell != "NULL":
                    return "row %d column %d: %s against NULL" % (index, column, cell)
                continue
            if not same_cell(cell, expected):
                return "row %d column %d: %s against %s" % (index, column, cell, expected)
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--vcqe", default="build/vcqe")
    parser.add_argument("--dir", required=True)
    parser.add_argument("--queries", type=int, default=100)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--mode", default="vectorized")
    arguments = parser.parse_args()
    random.seed(arguments.seed)

    connection = build_sqlite(arguments.dir)
    statements = []
    for name in ["q1", "q3", "q5", "q6", "q10"]:
        path = os.path.join("queries", name + ".sql")
        if os.path.exists(path):
            with open(path, "r", encoding="utf-8") as handle:
                statements.append(handle.read().strip())
    while len(statements) < arguments.queries + 5:
        statements.append(random_query(connection))

    failures = 0
    for number, sql in enumerate(statements):
        flat = " ".join(sql.split())
        try:
            expected = connection.execute(to_sqlite(flat)).fetchall()
        except sqlite3.Error as error:
            print("sqlite rejected: %s (%s)" % (flat, error))
            continue
        rows, error = run_vcqe(arguments.vcqe, arguments.dir, flat, arguments.mode)
        if rows is None:
            print("FAIL %d: %s\n  vcqe: %s" % (number, flat, error))
            failures += 1
            continue
        problem = compare(rows, expected, "order by" in flat)
        if problem is not None:
            print("FAIL %d: %s\n  %s" % (number, flat, problem))
            failures += 1
    print("%d queries, %d failures" % (len(statements), failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
