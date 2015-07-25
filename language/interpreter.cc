#include "interpreter.hpp"
#include "parser.hpp"
#include <sstream>

using namespace peglib;
using namespace std;

struct Eval
{
    static Value eval(const Ast& ast, shared_ptr<Environment> env) {
        switch (ast.tag) {
            case "STATEMENTS"_:          return eval_statements(ast, env);
            case "WHILE"_:               return eval_while(ast, env);
            case "IF"_:                  return eval_if(ast, env);
            case "FUNCTION"_:            return eval_function(ast, env);
            case "CALL"_:                return eval_call(ast, env);
            case "ASSIGNMENT"_:          return eval_assignment(ast, env);
            case "LOGICAL_OR"_:          return eval_logical_or(ast, env);
            case "LOGICAL_AND"_:         return eval_logical_and(ast, env);
            case "CONDITION"_:           return eval_condition(ast, env);
            case "UNARY_PLUS"_:          return eval_unary_plus(ast, env);
            case "UNARY_MINUS"_:         return eval_unary_minus(ast, env);
            case "UNARY_NOT"_:           return eval_unary_not(ast, env);
            case "ADDITIVE"_:
            case "MULTIPLICATIVE"_:      return eval_bin_expression(ast, env);
            case "IDENTIFIER"_:          return eval_identifier(ast, env);
            case "OBJECT"_:              return eval_object(ast, env);
            case "ARRAY"_:               return eval_array(ast, env);
            case "NUMBER"_:              return eval_number(ast, env);
            case "BOOLEAN"_:             return eval_bool(ast, env);
            case "INTERPOLATED_STRING"_: return eval_interpolated_string(ast, env);
        }

        if (ast.is_token) {
            return Value(string(ast.token));
        }

        // NOTREACHED
        throw logic_error("invalid Ast type");
    }

private:
    static Value eval_statements(const Ast& ast, shared_ptr<Environment> env) {
        if (ast.is_token) {
            return eval(ast, env);
        } else if (ast.nodes.empty()) {
            return Value();
        }
        auto it = ast.nodes.begin();
        while (it != ast.nodes.end() - 1) {
            eval(**it, env);
            ++it;
        }
        return eval(**it, env);
    }

    static Value eval_while(const Ast& ast, shared_ptr<Environment> env) {
        for (;;) {
            auto cond = eval(*ast.nodes[0], env);
            if (!cond.to_bool()) {
                break;
            }
            eval(*ast.nodes[1], env);
        }
        return Value();
    }

    static Value eval_if(const Ast& ast, shared_ptr<Environment> env) {
        const auto& nodes = ast.nodes;

        for (auto i = 0u; i < nodes.size(); i += 2) {
            if (i + 1 == nodes.size()) {
                return eval(*nodes[i], env);
            } else {
                auto cond = eval(*nodes[i], env);
                if (cond.to_bool()) {
                    return eval(*nodes[i + 1], env);
                }
            }
        }

        return Value();
    }

    static Value eval_function(const Ast& ast, shared_ptr<Environment> env) {
        std::vector<FunctionValue::Parameter> params;
        for (auto node: ast.nodes[0]->nodes) {
            auto mut = node->nodes[0]->token == "mut";
            const auto& name = node->nodes[1]->token;
            params.push_back({ name, mut });
        }

        auto body = ast.nodes[1];

        auto f = FunctionValue(
            params,
            [=](shared_ptr<Environment> callEnv) {
                callEnv->append_outer(env);
                return eval(*body, callEnv);
            }
        );

        return Value(std::move(f));
    };

    static Value eval_call(const Ast& ast, shared_ptr<Environment> env) {
        Value val = eval(*ast.nodes[0], env);

        for (auto i = 1u; i < ast.nodes.size(); i++) {
            const auto& n = *ast.nodes[i];
            if (n.original_tag == "ARGUMENTS"_) {
                // Function call
                const auto& f = val.to_function();
                const auto& params = f.data->params;
                const auto& args = n.nodes;
                if (params.size() <= args.size()) {
                    auto callEnv = make_shared<Environment>();

                    callEnv->initialize("self", val, false);

                    for (auto iprm = 0u; iprm < params.size(); iprm++) {
                        auto param = params[iprm];
                        auto arg = args[iprm];
                        auto val = eval(*arg, env);
                        callEnv->initialize(param.name, val, param.mut);
                    }

                    callEnv->initialize("__LINE__", Value((long)ast.line), false);
                    callEnv->initialize("__COLUMN__", Value((long)ast.column), false);

                    val = f.data->eval(callEnv);
                } else {
                    string msg = "arguments error...";
                    throw runtime_error(msg);
                }
            } else if (n.original_tag == "INDEX"_) {
                // Array reference
                const auto& arr = val.to_array();
                auto idx = eval(n, env).to_long();
                if (0 <= idx && idx < static_cast<long>(arr.values->size())) {
                    val = arr.values->at(idx);
                }
            } else if (n.original_tag == "DOT"_) {
                // Property
                auto name = n.token;
                auto prop = val.get_property(name);

                if (prop.get_type() == Value::Function) {
                    const auto& pf = prop.to_function();

                    auto f = FunctionValue(
                        pf.data->params,
                        [=](shared_ptr<Environment> callEnv) {
                            callEnv->initialize("this", val, false);
                            if (val.get_type() == Value::Object) {
                                callEnv->set_object(val.to_object());
                            }
                            return pf.data->eval(callEnv);
                        }
                    );

                    val = Value(std::move(f));
                } else {
                    val = prop;
                }
            } else {
                throw std::logic_error("invalid internal condition.");
            }
        }

        return val;
    }

    static Value eval_logical_or(const Ast& ast, shared_ptr<Environment> env) {
        if (ast.nodes.size() == 1) {
            return eval(*ast.nodes[0], env);
        } else {
            Value ret;
            for (auto node: ast.nodes) {
                ret = eval(*node, env);
                if (ret.to_bool()) {
                    return ret;
                }
            }
            return ret;
        }
    }

    static Value eval_logical_and(const Ast& ast, shared_ptr<Environment> env) {
        Value ret;
        for (auto node: ast.nodes) {
            ret = eval(*node, env);
            if (!ret.to_bool()) {
                return ret;
            }
        }
        return ret;
    }

    static Value eval_condition(const Ast& ast, shared_ptr<Environment> env) {
        if (ast.nodes.size() == 1) {
            return eval(*ast.nodes[0], env);
        } else {
            auto lhs = eval(*ast.nodes[0], env);
            auto ope = eval(*ast.nodes[1], env).to_string();
            auto rhs = eval(*ast.nodes[2], env);

            if (ope == "==") {
                return Value(lhs == rhs);
            } else if (ope == "!=") {
                return Value(lhs != rhs);
            } else if (ope == "<=") {
                return Value(lhs <= rhs);
            } else if (ope == "<") {
                return Value(lhs < rhs);
            } else if (ope == ">=") {
                return Value(lhs >= rhs);
            } else if (ope == ">") {
                return Value(lhs > rhs);
            } else {
                throw std::logic_error("invalid internal condition.");
            }
        }
    }

    static Value eval_unary_plus(const Ast& ast, shared_ptr<Environment> env) {
        if (ast.nodes.size() == 1) {
            return eval(*ast.nodes[0], env);
        } else {
            return eval(*ast.nodes[1], env);
        }
    }

    static Value eval_unary_minus(const Ast& ast, shared_ptr<Environment> env) {
        if (ast.nodes.size() == 1) {
            return eval(*ast.nodes[0], env);
        } else {
            return Value(eval(*ast.nodes[1], env).to_long() * -1);
        }
    }

    static Value eval_unary_not(const Ast& ast, shared_ptr<Environment> env) {
        if (ast.nodes.size() == 1) {
            return eval(*ast.nodes[0], env);
        } else {
            return Value(!eval(*ast.nodes[1], env).to_bool());
        }
    }

    static Value eval_bin_expression(const Ast& ast, shared_ptr<Environment> env) {
        auto ret = eval(*ast.nodes[0], env).to_long();
        for (auto i = 1u; i < ast.nodes.size(); i += 2) {
            auto val = eval(*ast.nodes[i + 1], env).to_long();
            auto ope = eval(*ast.nodes[i], env).to_string()[0];
            switch (ope) {
                case '+': ret += val; break;
                case '-': ret -= val; break;
                case '*': ret *= val; break;
                case '/': ret /= val; break;
                case '%': ret %= val; break;
            }
        }
        return Value(ret);
    }

    static Value eval_assignment(const Ast& ast, shared_ptr<Environment> env) {
        const auto& mut = ast.nodes[0]->token;
        const auto& var = ast.nodes[1]->token;
        auto val = eval(*ast.nodes[2], env);
        if (env->has(var)) {
            env->assign(var, val);
        } else {
            env->initialize(var, val, mut == "mut");
        }
        return val;
    };

    static Value eval_identifier(const Ast& ast, shared_ptr<Environment> env) {
        const auto& var = ast.token;
        return env->get(var);
    };

    static Value eval_object(const Ast& ast, shared_ptr<Environment> env) {
        ObjectValue obj;

        for (auto i = 0u; i < ast.nodes.size(); i++) {
            const auto& prop = *ast.nodes[i];
            const auto& name = prop.nodes[0]->token;
            auto val = eval(*prop.nodes[1], env);
            obj.properties->emplace(name, val);
        }

        return Value(std::move(obj));
    }

    static Value eval_array(const Ast& ast, shared_ptr<Environment> env) {
        ArrayValue arr;

        for (auto i = 0u; i < ast.nodes.size(); i++) {
            auto expr = ast.nodes[i];
            auto val = eval(*expr, env);
            arr.values->push_back(val);
        }

        return Value(std::move(arr));
    }

    static Value eval_number(const Ast& ast, shared_ptr<Environment> env) {
        return Value(stol(ast.token));
    };

    static Value eval_bool(const Ast& ast, shared_ptr<Environment> env) {
        return Value(ast.token == "true");
    };

    static Value eval_interpolated_string(const Ast& ast, shared_ptr<Environment> env) {
        string s;
        for (auto node: ast.nodes) {
            const auto& val = eval(*node, env);
            s += val.str();
        }
        return Value(std::move(s));
    };
};

bool run(
    const string&           path,
    shared_ptr<Environment> env,
    const char*             expr,
    size_t                  len,
    Value&                  val,
    string&                 msg,
    bool                    print_ast)
{
    try {
        shared_ptr<Ast> ast;

        auto& parser = get_parser();

        parser.log = [&](size_t ln, size_t col, const string& err_msg) {
            stringstream ss;
            ss << path << ":" << ln << ":" << col << ": " << err_msg << endl;
            msg = ss.str();
        };

        if (parser.parse_n(expr, len, ast)) {
            if (print_ast) {
                ast->print();
            }

            val = Eval::eval(*ast, env);
            return true;
        }
    } catch (runtime_error& e) {
        msg = e.what();
    }

    return false;
}

// vim: et ts=4 sw=4 cin cino={1s ff=unix
