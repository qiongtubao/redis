#!/usr/bin/env python3
import glob
import json
import os
import argparse

ARG_TYPES = {
    "string": "ARG_TYPE_STRING",
    "integer": "ARG_TYPE_INTEGER",
    "double": "ARG_TYPE_DOUBLE",
    "key": "ARG_TYPE_KEY",
    "pattern": "ARG_TYPE_PATTERN",
    "unix-time": "ARG_TYPE_UNIX_TIME",
    "pure-token": "ARG_TYPE_PURE_TOKEN",
    "oneof": "ARG_TYPE_ONEOF",
    "block": "ARG_TYPE_BLOCK",
}

GROUPS = {
    "generic": "COMMAND_GROUP_GENERIC",
    "string": "COMMAND_GROUP_STRING",
    "list": "COMMAND_GROUP_LIST",
    "set": "COMMAND_GROUP_SET",
    "sorted_set": "COMMAND_GROUP_SORTED_SET",
    "hash": "COMMAND_GROUP_HASH",
    "pubsub": "COMMAND_GROUP_PUBSUB",
    "transactions": "COMMAND_GROUP_TRANSACTIONS",
    "connection": "COMMAND_GROUP_CONNECTION",
    "server": "COMMAND_GROUP_SERVER",
    "scripting": "COMMAND_GROUP_SCRIPTING",
    "hyperloglog": "COMMAND_GROUP_HYPERLOGLOG",
    "cluster": "COMMAND_GROUP_CLUSTER",
    "sentinel": "COMMAND_GROUP_SENTINEL",
    "geo": "COMMAND_GROUP_GEO",
    "stream": "COMMAND_GROUP_STREAM",
    "bitmap": "COMMAND_GROUP_BITMAP",
}


def get_optional_desc_string(desc, field, force_uppercase=False):   #字典获得字符串
    v = desc.get(field, None)   # 字典获得属性
    if v and force_uppercase:   # 强制转大写
        v = v.upper()
    ret = "\"%s\"" % v if v else "NULL" # 字符串前后加双引号 不存在就是NULL
    return ret.replace("\n", "\\n") #转义字符


def check_command_args_key_specs(args, command_key_specs_index_set, command_arg_key_specs_index_set): #验证 Redis 命令参数中引用的 key_spec 索引是否合法且一致
    if not args:        # 空参数跳过
        return True

    for arg in args:                        #遍历参数
        if arg.key_spec_index is not None:                  #参数的key_spec_index 非空
            assert isinstance(arg.key_spec_index, int)      #索引值必须为整数    重要！！！

            if arg.key_spec_index not in command_key_specs_index_set:   #必须在command_key_specs_index_set索引集合里面
                print("command: %s arg: %s key_spec_index error" % (command.fullname(), arg.name))
                return False

            command_arg_key_specs_index_set.add(arg.key_spec_index)     #添加到command_arg_key_specs_index_set集合里

        if not check_command_args_key_specs(arg.subargs, command_key_specs_index_set, command_arg_key_specs_index_set): #子参数递归检查
            return False

    return True

def check_command_key_specs(command): #对 Redis 命令的 key_specs 和参数引用关系进行完整性与一致性验证。
    if not command.key_specs:           #没有key specs 跳过
        return True

    assert isinstance(command.key_specs, list)  #command.key_specs类型必须是数组

    for cmd_key_spec in command.key_specs:      #遍历command.key_specs数组
        if "flags" not in cmd_key_spec:         #必须有flags 字段
            print("command: %s key_specs missing flags" % command.fullname())
            return False

        if "NOT_KEY" in cmd_key_spec["flags"]:  # flags 属性有NOT_KEY 就跳出
            # Like SUNSUBSCRIBE / SPUBLISH / SSUBSCRIBE
            return True

    command_key_specs_index_set = set(range(len(command.key_specs))) #生成所有合法的 key_spec 索引集合。 比如参数长度为3 就生存0，1，2
    command_arg_key_specs_index_set = set()                         #   收集实际索引

    # Collect key_spec used for each arg, including arg.subarg
    if not check_command_args_key_specs(command.args, command_key_specs_index_set, command_arg_key_specs_index_set): #递归检查所有参数对 key_spec 的引用
        return False

    # Check if we have key_specs not used
    if command_key_specs_index_set != command_arg_key_specs_index_set:  # 检查是否有未使用的 key_spec
        print("command: %s may have unused key_spec" % command.fullname())
        return False

    return True


# Globals
subcommands = {}  # container_name -> dict(subcommand_name -> Subcommand) - Only subcommands
commands = {}  # command_name -> Command - Only commands


class KeySpec(object):
    def __init__(self, spec):
        self.spec = spec

    def struct_code(self):          # 生成一个字符串
        def _flags_code():          # 生成flags  
            s = ""
            for flag in self.spec.get("flags", []):  #flags=["RW", "ACCESS"] 返回CMD_KEY_RW|CMD_KEY_ACCESS
                s += "CMD_KEY_%s|" % flag
            return s[:-1] if s else 0               #没有返回0

        def _begin_search_code():                   #生成begin_search
            if self.spec["begin_search"].get("index"):              #如果有index
                return "KSPEC_BS_INDEX,.bs.index={%d}" % (          # KSPEC_BS_INDEX,.bs.index={1}
                    self.spec["begin_search"]["index"]["pos"]
                )
            elif self.spec["begin_search"].get("keyword"):
                return "KSPEC_BS_KEYWORD,.bs.keyword={\"%s\",%d}" % ( #.bs.keyword={"KEYS",1}
                    self.spec["begin_search"]["keyword"]["keyword"],
                    self.spec["begin_search"]["keyword"]["startfrom"],
                )
            elif "unknown" in self.spec["begin_search"]:            #无法识别 KSPEC_BS_UNKNOWN,{{0}}
                return "KSPEC_BS_UNKNOWN,{{0}}"
            else:
                print("Invalid begin_search! value=%s" % self.spec["begin_search"]) #都没有终止
                exit(1)

        def _find_keys_code():                          #生成“查找 key”策略
            if self.spec["find_keys"].get("range"):
                return "KSPEC_FK_RANGE,.fk.range={%d,%d,%d}" % (    #KSPEC_FK_RANGE,.fk.range={1,1,1}
                    self.spec["find_keys"]["range"]["lastkey"],
                    self.spec["find_keys"]["range"]["step"],
                    self.spec["find_keys"]["range"]["limit"]
                )
            elif self.spec["find_keys"].get("keynum"):
                return "KSPEC_FK_KEYNUM,.fk.keynum={%d,%d,%d}" % (  #KSPEC_FK_KEYNUM,.fk.keynum={2,1,1}
                    self.spec["find_keys"]["keynum"]["keynumidx"],
                    self.spec["find_keys"]["keynum"]["firstkey"],
                    self.spec["find_keys"]["keynum"]["step"]
                )
            elif "unknown" in self.spec["find_keys"]:               #KSPEC_FK_UNKNOWN,{{0}}
                return "KSPEC_FK_UNKNOWN,{{0}}"
            else:
                print("Invalid find_keys! value=%s" % self.spec["find_keys"])
                exit(1)

        return "%s,%s,%s,%s" % (                                #最终生成一个逗号分隔的 C 结构体字段初始化字符串。
            get_optional_desc_string(self.spec, "notes"),       # 获得spec["notes"]字符串
            _flags_code(),                                      # CMD_KEY_XX
            _begin_search_code(),                               # KSPEC_BS_INDEX,.bs.index={1}
            _find_keys_code()                                   # KSPEC_FK_KEYNUM,.fk.keynum={2,1,1}
        )


def verify_no_dup_names(container_fullname, args):              #检查一组命令参数（或子命令、选项等）中是否存在重复的名称，确保在同一作用域内所有参数名唯一
    name_list = [arg.name for arg in args]                      # 获得所有参数名
    name_set = set(name_list)                                   # 转成集合
    if len(name_list) != len(name_set):                         # 个数对比 不一样就表示有同名
        print("{}: Dup argument names: {}".format(container_fullname, name_list))
        exit(1)


class Argument(object):                                     #命令参数结构  conf文件中arguments属性解析成argument对象 
    def __init__(self, parent_name, desc):                  
        self.parent_name = parent_name                      # 父级名称（如命令名）  
        self.desc = desc                                    # 描述参数原始描述字典
        self.name = self.desc["name"].lower()               # 参数名（小写）
        if "_" in self.name:                                # 名字不能有下划线
            print("{}: name ({}) should not contain underscores".format(self.fullname(), self.name))
            exit(1)
        self.type = self.desc["type"]                       #解析出类型
        self.key_spec_index = self.desc.get("key_spec_index", None) #关联的 key_spec 索引
        self.subargs = []                                   #子参数
        if self.type in ["oneof", "block"]:                 #oneof类型 互斥选项 比如NX｜XX ，block类型 一组可选参数
            self.display = None                             #
            for subdesc in self.desc["arguments"]:          # 参数遍历
                self.subargs.append(Argument(self.fullname(), subdesc))  #创建子参数
            if len(self.subargs) < 2:                       # 参数选项小于2  异常
                print("{}: oneof or block arg contains less than two subargs".format(self.fullname()))
                exit(1)
            verify_no_dup_names(self.fullname(), self.subargs)  #检查参数是否重复
        else:
            self.display = self.desc.get("display")          #显示文本

    def fullname(self):                     #对象全名 SET nx
        return ("%s %s" % (self.parent_name, self.name)).replace("-", "_")

    def struct_name(self):                  #结构体  SET nx => SET_nx_Arg
        return "%s_Arg" % (self.fullname().replace(" ", "_"))

    def subarg_table_name(self):            #子表名称   SET_nx_Subargs
        assert self.subargs
        return "%s_Subargs" % (self.fullname().replace(" ", "_"))

    def struct_code(self):                  #初始代码 MAKE_ARG()
        """
        Output example:
        MAKE_ARG("expiration",ARG_TYPE_ONEOF,-1,NULL,NULL,NULL,CMD_ARG_OPTIONAL,5,NULL),.subargs=GETEX_expiration_Subargs
        """

        def _flags_code():                          #flags ,optional => CMD_ARG_OPTIONAL,
            s = ""
            if self.desc.get("optional", False):
                s += "CMD_ARG_OPTIONAL|"
            if self.desc.get("multiple", False):
                s += "CMD_ARG_MULTIPLE|"
            if self.desc.get("multiple_token", False):
                assert self.desc.get("multiple", False)  # Sanity
                s += "CMD_ARG_MULTIPLE_TOKEN|"
            return s[:-1] if s else "CMD_ARG_NONE"  #什么都没有返回CMD_ARG_NONE

        s = "MAKE_ARG(\"%s\",%s,%d,%s,%s,%s,%s,%d,%s)" % (
            self.name,                                      #名称
            ARG_TYPES[self.type],                           #类型
            self.desc.get("key_spec_index", -1),            # 索引默认-1
            get_optional_desc_string(self.desc, "token", force_uppercase=True), # token 强转大
            get_optional_desc_string(self.desc, "summary"), # summary
            get_optional_desc_string(self.desc, "since"),   # since
            _flags_code(),
            len(self.subargs),                              #slef.subargs 长度
            get_optional_desc_string(self.desc, "deprecated_since"),
        )
        if "display" in self.desc:
            s += ",.display_text=\"%s\"" % self.desc["display"].lower()
        if self.subargs:
            s += ",.subargs=%s" % self.subarg_table_name()

        return s

    def write_internal_structs(self, f):        # 定义一个COMMAND_ARG 对象
        if self.subargs:
            for subarg in self.subargs:
                subarg.write_internal_structs(f)

            f.write("/* %s argument table */\n" % self.fullname())
            f.write("struct COMMAND_ARG %s[] = {\n" % self.subarg_table_name())
            for subarg in self.subargs:
                f.write("{%s},\n" % subarg.struct_code())
            f.write("};\n\n")


def to_c_name(str):
    return str.replace(":", "").replace(".", "_").replace("$", "_")\
        .replace("^", "_").replace("*", "_").replace("-", "_") \
        .replace("\\", "_").replace("+", "_")


class ReplySchema(object):
    def __init__(self, name, desc):                 #创建对象  
        self.name = to_c_name(name) 
        self.schema = {}                            #检查看数据是否规范
        if desc.get("type") == "object":           # type 是object 的话 必须有 properties 和additionalProperties
            if desc.get("properties") and desc.get("additionalProperties") is None:
                print("%s: Any object that has properties should have the additionalProperties field" % self.name)
                exit(1)
        elif desc.get("type") == "array":         # type 是array 的话 必须有items 且items 对象是list 且minItems和maxItems 属性为None
            if desc.get("items") and isinstance(desc["items"], list) and any([desc.get(k) is None for k in ["minItems", "maxItems"]]):
                print("%s: Any array that has items should have the minItems and maxItems fields" % self.name)
                exit(1)
        for k, v in desc.items():               #如果是map就继续
            if isinstance(v, dict):             # value是json对象就生成一个ReplySchema
                self.schema[k] = ReplySchema("%s_%s" % (self.name, k), v)
            elif isinstance(v, list):           # value是个数组 则是ReplySchema数组
                self.schema[k] = []
                for i, subdesc in enumerate(v):
                    self.schema[k].append(ReplySchema("%s_%s_%i" % (self.name, k,i), subdesc))
            else:                               # 其他情况就是普通值
                self.schema[k] = v
    
    def write(self, f):                         #写入文件
        def struct_code(name, k, v):            
            if isinstance(v, ReplySchema):      #value是replySchema对象类型
                t = "JSON_TYPE_OBJECT"
                vstr = ".value.object=&%s" % name
            elif isinstance(v, list):           #value是数组
                t = "JSON_TYPE_ARRAY"
                vstr = ".value.array={.objects=%s,.length=%d}" % (name, len(v))
            elif isinstance(v, bool):           #value是布尔值
                t = "JSON_TYPE_BOOLEAN"
                vstr = ".value.boolean=%d" % int(v)
            elif isinstance(v, str):            #value是字符串
                t = "JSON_TYPE_STRING"
                vstr = ".value.string=\"%s\"" % v
            elif isinstance(v, int):            #value是int
                t = "JSON_TYPE_INTEGER"
                vstr = ".value.integer=%d" % v
            
            return "%s,%s,%s" % (t, json.dumps(k), vstr)

        for k, v in self.schema.items():
            if isinstance(v, ReplySchema):
                v.write(f)
            elif isinstance(v, list):
                for i, schema in enumerate(v):
                    schema.write(f)
                name = to_c_name("%s_%s" % (self.name, k))
                f.write("/* %s array reply schema */\n" % name)
                f.write("struct jsonObject *%s[] = {\n" % name)
                for i, schema in enumerate(v):
                    f.write("&%s,\n" % schema.name)
                f.write("};\n\n")
            
        f.write("/* %s reply schema */\n" % self.name)
        f.write("struct jsonObjectElement %s_elements[] = {\n" % self.name)
        for k, v in self.schema.items():
            name = to_c_name("%s_%s" % (self.name, k))
            f.write("{%s},\n" % struct_code(name, k, v))
        f.write("};\n\n")
        f.write("struct jsonObject %s = {%s_elements,.length=%d};\n\n" % (self.name, self.name, len(self.schema)))


class Command(object):              #这是整个命令描述、验证和 C 代码生成系统的顶级容器类，它整合了 Argument、ReplySchema 和 KeySpec，最终生成完整的 C 结构体定义
    def __init__(self, name, desc):
        self.name = name.upper()    # 强制大写
        self.desc = desc            # 字典
        self.group = self.desc["group"] # 字符串
        self.key_specs = self.desc.get("key_specs", []) #数组
        self.subcommands = []       #子命令数组
        self.args = []              #参数数组
        for arg_desc in self.desc.get("arguments", []): #arguments 数组
            self.args.append(Argument(self.fullname(), arg_desc))
        verify_no_dup_names(self.fullname(), self.args) #是否有重名
        self.reply_schema = None
        if "reply_schema" in self.desc:                 #如果有属性reply_schema就创建ReplySchema对象
            self.reply_schema = ReplySchema(self.reply_schema_name(), self.desc["reply_schema"])

    def fullname(self):                         #命令全名
        return self.name.replace("-", "_").replace(":", "")

    def return_types_table_name(self):          
        return "%s_ReturnInfo" % self.fullname().replace(" ", "_")

    def subcommand_table_name(self):            #子命令
        assert self.subcommands
        return "%s_Subcommands" % self.name

    def history_table_name(self):               #历史记录名
        return "%s_History" % (self.fullname().replace(" ", "_"))

    def tips_table_name(self):                  #提示表名
        return "%s_Tips" % (self.fullname().replace(" ", "_"))

    def arg_table_name(self):                   #参数表名
        return "%s_Args" % (self.fullname().replace(" ", "_"))

    def key_specs_table_name(self):             #键规范表名
        return "%s_Keyspecs" % (self.fullname().replace(" ", "_"))

    def reply_schema_name(self):                # Schema 名
        return "%s_ReplySchema" % (self.fullname().replace(" ", "_"))

    def struct_name(self):                      #命令结构体名
        return "%s_Command" % (self.fullname().replace(" ", "_"))

    def history_code(self):                     #历史表名
        if not self.desc.get("history"):
            return ""
        s = ""
        for tupl in self.desc["history"]:
            s += "{\"%s\",\"%s\"},\n" % (tupl[0], tupl[1]) # 版本， 记录
        return s

    def num_history(self):                      #历史个数
        if not self.desc.get("history"):
            return 0
        return len(self.desc["history"])

    def tips_code(self):                        #生成tips 代码
        if not self.desc.get("command_tips"):
            return ""
        s = ""
        for hint in self.desc["command_tips"]:
            s += "\"%s\",\n" % hint.lower()
        return s

    def num_tips(self):                         #tips 个数
        if not self.desc.get("command_tips"):
            return 0
        return len(self.desc["command_tips"])

    def key_specs_code(self):                   #生成key规范代码
        s = ""
        for spec in self.key_specs:
            s += "{%s}," % KeySpec(spec).struct_code()
        return s[:-1]


    def struct_code(self):                      #生成MAKE_CMD代码
        """
        Output example:
        MAKE_CMD("set","Set the string value of a key","O(1)","1.0.0",CMD_DOC_NONE,NULL,NULL,"string",COMMAND_GROUP_STRING,SET_History,4,SET_Tips,0,setCommand,-3,CMD_WRITE|CMD_DENYOOM,ACL_CATEGORY_STRING,SET_Keyspecs,1,setGetKeys,5),.args=SET_Args
        """

        def _flags_code():
            s = ""
            for flag in self.desc.get("command_flags", []):
                s += "CMD_%s|" % flag
            return s[:-1] if s else 0

        def _acl_categories_code():
            s = ""
            for cat in self.desc.get("acl_categories", []):
                s += "ACL_CATEGORY_%s|" % cat
            return s[:-1] if s else 0

        def _doc_flags_code():
            s = ""
            for flag in self.desc.get("doc_flags", []):
                s += "CMD_DOC_%s|" % flag
            return s[:-1] if s else "CMD_DOC_NONE"

        s = "MAKE_CMD(\"%s\",%s,%s,%s,%s,%s,%s,%s,%s,%s,%d,%s,%d,%s,%d,%s,%s,%s,%d,%s,%d)," % (
            self.name.lower(),
            get_optional_desc_string(self.desc, "summary"),
            get_optional_desc_string(self.desc, "complexity"),
            get_optional_desc_string(self.desc, "since"),
            _doc_flags_code(),
            get_optional_desc_string(self.desc, "replaced_by"),
            get_optional_desc_string(self.desc, "deprecated_since"),
            "\"%s\"" % self.group,
            GROUPS[self.group],
            self.history_table_name(),
            self.num_history(),
            self.tips_table_name(),
            self.num_tips(),
            self.desc.get("function", "NULL"),
            self.desc["arity"],
            _flags_code(),
            _acl_categories_code(),
            self.key_specs_table_name(),
            len(self.key_specs),
            self.desc.get("get_keys_function", "NULL"),
            len(self.args),
        )

        if self.subcommands:
            s += ".subcommands=%s," % self.subcommand_table_name()

        if self.args:
            s += ".args=%s," % self.arg_table_name()

        if self.reply_schema and args.with_reply_schema:
            s += ".reply_schema=&%s," % self.reply_schema_name()

        return s[:-1]

    def write_internal_structs(self, f):
        if self.subcommands:
            subcommand_list = sorted(self.subcommands, key=lambda cmd: cmd.name)
            for subcommand in subcommand_list:
                subcommand.write_internal_structs(f)

            f.write("/* %s command table */\n" % self.fullname())
            f.write("struct COMMAND_STRUCT %s[] = {\n" % self.subcommand_table_name())
            for subcommand in subcommand_list:
                f.write("{%s},\n" % subcommand.struct_code())
            f.write("{0}\n")
            f.write("};\n\n")

        f.write("/********** %s ********************/\n\n" % self.fullname())

        f.write("#ifndef SKIP_CMD_HISTORY_TABLE\n")
        f.write("/* %s history */\n" % self.fullname())
        code = self.history_code()
        if code:
            f.write("commandHistory %s[] = {\n" % self.history_table_name())
            f.write("%s" % code)
            f.write("};\n")
        else:
            f.write("#define %s NULL\n" % self.history_table_name())
        f.write("#endif\n\n")

        f.write("#ifndef SKIP_CMD_TIPS_TABLE\n")
        f.write("/* %s tips */\n" % self.fullname())
        code = self.tips_code()
        if code:
            f.write("const char *%s[] = {\n" % self.tips_table_name())
            f.write("%s" % code)
            f.write("};\n")
        else:
            f.write("#define %s NULL\n" % self.tips_table_name())
        f.write("#endif\n\n")

        f.write("#ifndef SKIP_CMD_KEY_SPECS_TABLE\n")
        f.write("/* %s key specs */\n" % self.fullname())
        code = self.key_specs_code()
        if code:
            f.write("keySpec %s[%d] = {\n" % (self.key_specs_table_name(), len(self.key_specs)))
            f.write("%s\n" % code)
            f.write("};\n")
        else:
            f.write("#define %s NULL\n" % self.key_specs_table_name())
        f.write("#endif\n\n")

        if self.args:
            for arg in self.args:
                arg.write_internal_structs(f)

            f.write("/* %s argument table */\n" % self.fullname())
            f.write("struct COMMAND_ARG %s[] = {\n" % self.arg_table_name())
            for arg in self.args:
                f.write("{%s},\n" % arg.struct_code())
            f.write("};\n\n")

        if self.reply_schema and args.with_reply_schema:
            self.reply_schema.write(f)


class Subcommand(Command):
    def __init__(self, name, desc):
        self.container_name = desc["container"].upper()
        super(Subcommand, self).__init__(name, desc)

    def fullname(self):
        return "%s %s" % (self.container_name, self.name.replace("-", "_").replace(":", ""))


def create_command(name, desc):
    flags = desc.get("command_flags")
    if flags and "EXPERIMENTAL" in flags:
        print("Command %s is experimental, skipping..." % name)
        return

    if desc.get("container"):
        cmd = Subcommand(name.upper(), desc)
        subcommands.setdefault(desc["container"].upper(), {})[name] = cmd
    else:
        cmd = Command(name.upper(), desc)
        commands[name.upper()] = cmd


# MAIN

# Figure out where the sources are
srcdir = os.path.abspath(os.path.dirname(os.path.abspath(__file__)) + "/../src")

parser = argparse.ArgumentParser()
parser.add_argument('--with-reply-schema', action='store_true')
args = parser.parse_args()

# Create all command objects
print("Processing json files...")
for filename in glob.glob('%s/commands/*.json' % srcdir):
    with open(filename, "r") as f:
        try:
            d = json.load(f)
            for name, desc in d.items():
                create_command(name, desc)
        except json.decoder.JSONDecodeError as err:
            print("Error processing %s: %s" % (filename, err))
            exit(1)

# Link subcommands to containers
print("Linking container command to subcommands...")
for command in commands.values():
    assert command.group
    if command.name not in subcommands:
        continue
    for subcommand in subcommands[command.name].values():
        assert not subcommand.group or subcommand.group == command.group
        subcommand.group = command.group
        command.subcommands.append(subcommand)

check_command_error_counter = 0  # An error counter is used to count errors in command checking.

print("Checking all commands...")
for command in commands.values():
    if not check_command_key_specs(command):
        check_command_error_counter += 1

if check_command_error_counter != 0:
    print("Error: There are errors in the commands check, please check the above logs.")
    exit(1)

commands_filename = "commands_with_reply_schema" if args.with_reply_schema else "commands"
print("Generating %s.def..." % commands_filename)
with open("%s/%s.def" % (srcdir, commands_filename), "w") as f:
    f.write("/* Automatically generated by %s, do not edit. */\n\n" % os.path.basename(__file__))
    f.write(
"""
/* We have fabulous commands from
 * the fantastic
 * Redis Command Table! */

/* Must match redisCommandGroup */
const char *COMMAND_GROUP_STR[] = {
    "generic",
    "string",
    "list",
    "set",
    "sorted-set",
    "hash",
    "pubsub",
    "transactions",
    "connection",
    "server",
    "scripting",
    "hyperloglog",
    "cluster",
    "sentinel",
    "geo",
    "stream",
    "bitmap",
    "module"
};

const char *commandGroupStr(int index) {
    return COMMAND_GROUP_STR[index];
}
"""
    )

    command_list = sorted(commands.values(), key=lambda cmd: (cmd.group, cmd.name))
    for command in command_list:
        command.write_internal_structs(f)

    f.write("/* Main command table */\n")
    f.write("struct COMMAND_STRUCT redisCommandTable[] = {\n")
    curr_group = None
    for command in command_list:
        if curr_group != command.group:
            curr_group = command.group
            f.write("/* %s */\n" % curr_group)
        f.write("{%s},\n" % command.struct_code())
    f.write("{0}\n")
    f.write("};\n")

print("All done, exiting.")
