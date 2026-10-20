/****************************************************************************

    flow5 application
    Copyright © 2025 André Deperrois

    This file is part of flow5.

    flow5 is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License,
    or (at your option) any later version.

    flow5 is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty
    of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
    See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with flow5.
    If not, see <https://www.gnu.org/licenses/>.


*****************************************************************************/

#include <iostream>

#include <QJsonDocument>
#include <QUrl>

#include "mcpserver.h"

using namespace mcp;


namespace
{
    char const *s_LevelNames[] = {"debug", "info", "notice", "warning", "error", "critical", "alert", "emergency"};

    QString idKey(QJsonValue const &id)
    {
        if(id.isString()) return "s:" + id.toString();
        return "n:" + QString::number(id.toDouble(), 'g', 17);
    }

    QString compact(QJsonObject const &obj)
    {
        return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    }
}


QJsonObject mcp::json(char const *literal)
{
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray(literal), &err);
    if(err.error!=QJsonParseError::NoError || !doc.isObject())
    {
        std::cerr << "flow5-mcp: malformed built-in JSON: " << err.errorString().toStdString() << "\n" << literal << std::endl;
        std::abort();
    }
    return doc.object();
}


QString mcp::argString(QJsonObject const &args, QString const &key)
{
    QJsonValue v = args.value(key);
    if(!v.isString()) throw ToolError("Missing or invalid string argument: "+key);
    return v.toString();
}


QString mcp::argString(QJsonObject const &args, QString const &key, QString const &defaultValue)
{
    if(!args.contains(key) || args.value(key).isNull()) return defaultValue;
    return argString(args, key);
}


double mcp::argDouble(QJsonObject const &args, QString const &key)
{
    QJsonValue v = args.value(key);
    if(!v.isDouble()) throw ToolError("Missing or invalid numeric argument: "+key);
    return v.toDouble();
}


double mcp::argDouble(QJsonObject const &args, QString const &key, double defaultValue)
{
    if(!args.contains(key) || args.value(key).isNull()) return defaultValue;
    return argDouble(args, key);
}


int mcp::argInt(QJsonObject const &args, QString const &key)
{
    double d = argDouble(args, key);
    if(d!=double(int(d))) throw ToolError("Argument must be an integer: "+key);
    return int(d);
}


int mcp::argInt(QJsonObject const &args, QString const &key, int defaultValue)
{
    if(!args.contains(key) || args.value(key).isNull()) return defaultValue;
    return argInt(args, key);
}


bool mcp::argBool(QJsonObject const &args, QString const &key, bool defaultValue)
{
    if(!args.contains(key) || args.value(key).isNull()) return defaultValue;
    QJsonValue v = args.value(key);
    if(!v.isBool()) throw ToolError("Invalid boolean argument: "+key);
    return v.toBool();
}


QJsonArray mcp::argArray(QJsonObject const &args, QString const &key)
{
    QJsonValue v = args.value(key);
    if(!v.isArray()) throw ToolError("Missing or invalid array argument: "+key);
    return v.toArray();
}


void Context::progress(double done, double total, QString const &message)
{
    if(!m_pServer || m_ProgressToken.isUndefined() || m_ProgressToken.isNull()) return;
    QJsonObject params;
    params["progressToken"] = m_ProgressToken;
    params["progress"] = done;
    if(total>0.0) params["total"] = total;
    if(!message.isEmpty()) params["message"] = message;
    m_pServer->notify("notifications/progress", params);
}


void Context::log(enumLogLevel level, QString const &message)
{
    if(!m_pServer || int(level)<m_pServer->m_LogLevel.load()) return;
    QJsonObject params;
    params["level"] = s_LevelNames[level];
    params["logger"] = "flow5";
    params["data"] = message;
    m_pServer->notify("notifications/message", params);
}


void Context::setCancelHook(std::function<void()> hook)
{
    std::lock_guard<std::mutex> lck(m_HookMutex);
    m_CancelHook = std::move(hook);
}


Server::Server(QString const &name, QString const &version, QString const &instructions)
    : m_Name(name), m_Version(version), m_Instructions(instructions)
{
    m_ProtocolVersion = supportedProtocolVersions().front();
}


QStringList Server::supportedProtocolVersions()
{
    // most recent first
    return {"2025-06-18", "2025-03-26", "2024-11-05"};
}


void Server::addTool(Tool const &tool)                     {m_Tools.push_back(tool);}
void Server::addResourceTemplate(ResourceTemplate const &t) {m_Templates.push_back(t);}
void Server::addPrompt(Prompt const &prompt)               {m_Prompts.push_back(prompt);}


int Server::run(int outFd)
{
    m_pOut = fdopen(outFd, "w");
    if(!m_pOut)
    {
        std::cerr << "flow5-mcp: could not open the protocol output stream" << std::endl;
        return 1;
    }

    std::thread worker(&Server::workerLoop, this);
    readerLoop();

    // the client has gone: cancel the running and queued requests so that the worker exits promptly
    {
        std::lock_guard<std::mutex> lck(m_ActiveMutex);
        for(auto &it : m_Active)
        {
            it.second->m_bCancelled = true;
            std::lock_guard<std::mutex> hlck(it.second->m_HookMutex);
            if(it.second->m_CancelHook) it.second->m_CancelHook();
        }
    }
    {
        std::lock_guard<std::mutex> lck(m_QueueMutex);
        m_bStop = true;
    }
    m_QueueCv.notify_all();
    worker.join();

    fclose(m_pOut);
    m_pOut = nullptr;
    return 0;
}


void Server::readerLoop()
{
    std::string line;
    while(std::getline(std::cin, line))
    {
        if(!line.empty() && line.back()=='\r') line.pop_back();
        if(line.find_first_not_of(" \t")==std::string::npos) continue;

        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(line), &err);
        if(err.error!=QJsonParseError::NoError)
        {
            sendError(QJsonValue::Null, PARSE_ERROR, "Parse error: "+err.errorString());
            continue;
        }

        if(doc.isArray())
        {
            // JSON-RPC batch (allowed by protocol 2025-03-26); each element is answered individually
            QJsonArray batch = doc.array();
            if(batch.isEmpty()) sendError(QJsonValue::Null, INVALID_REQUEST, "Empty batch");
            for(QJsonValue const &v : batch)
            {
                if(v.isObject()) handleMessage(v.toObject());
                else sendError(QJsonValue::Null, INVALID_REQUEST, "Invalid request");
            }
        }
        else if(doc.isObject())
            handleMessage(doc.object());
        else
            sendError(QJsonValue::Null, INVALID_REQUEST, "Invalid request");
    }
}


void Server::handleMessage(QJsonObject const &msg)
{
    QJsonValue id = msg.value("id");
    bool bHasId = msg.contains("id") && (id.isString() || id.isDouble());

    if(msg.value("jsonrpc").toString()!="2.0")
    {
        sendError(bHasId ? id : QJsonValue(QJsonValue::Null), INVALID_REQUEST, "jsonrpc must be \"2.0\"");
        return;
    }

    if(!msg.contains("method"))
    {
        // a response to a server-initiated request; this server sends none, so ignore it
        return;
    }

    QString method = msg.value("method").toString();
    QJsonObject params = msg.value("params").toObject();

    if(!bHasId)
    {
        if(msg.contains("id")) sendError(QJsonValue::Null, INVALID_REQUEST, "Invalid request id");
        else handleNotification(method, params);
        return;
    }

    if(method=="ping")
    {
        // answered from the reader thread so that it works while an analysis is running
        sendResult(id, QJsonObject());
        return;
    }

    PendingRequest req;
    req.id = id;
    req.method = method;
    req.params = params;
    req.ctx = std::make_shared<Context>();
    req.ctx->m_pServer = this;
    req.ctx->m_ProgressToken = params.value("_meta").toObject().value("progressToken");

    {
        std::lock_guard<std::mutex> lck(m_ActiveMutex);
        m_Active[idKey(id)] = req.ctx;
    }
    {
        std::lock_guard<std::mutex> lck(m_QueueMutex);
        m_Queue.push_back(req);
    }
    m_QueueCv.notify_one();
}


void Server::handleNotification(QString const &method, QJsonObject const &params)
{
    if(method=="notifications/initialized")
    {
        m_bInitialized = true;
    }
    else if(method=="notifications/cancelled")
    {
        std::shared_ptr<Context> ctx;
        {
            std::lock_guard<std::mutex> lck(m_ActiveMutex);
            auto it = m_Active.find(idKey(params.value("requestId")));
            if(it!=m_Active.end()) ctx = it->second;
        }
        if(ctx)
        {
            ctx->m_bCancelled = true;
            std::lock_guard<std::mutex> lck(ctx->m_HookMutex);
            if(ctx->m_CancelHook) ctx->m_CancelHook();
        }
    }
    // other notifications, e.g. roots/list_changed, are not used
}


void Server::workerLoop()
{
    while(true)
    {
        PendingRequest req;
        {
            std::unique_lock<std::mutex> lck(m_QueueMutex);
            m_QueueCv.wait(lck, [this]{return m_bStop || !m_Queue.empty();});
            if(m_Queue.empty()) return; // stopping and nothing left to process
            req = m_Queue.front();
            m_Queue.pop_front();
        }

        QJsonObject result;
        int errcode = 0;
        QString errmsg;

        if(!req.ctx->isCancelled())
        {
            try
            {
                result = dispatch(req.method, req.params, *req.ctx);
            }
            catch(RpcError const &e)
            {
                errcode = e.code();
                errmsg = QString::fromStdString(e.what());
            }
            catch(std::exception const &e)
            {
                errcode = INTERNAL_ERROR;
                errmsg = QString::fromStdString(e.what());
            }
        }

        {
            std::lock_guard<std::mutex> lck(m_ActiveMutex);
            m_Active.erase(idKey(req.id));
        }

        // no response is sent for a cancelled request
        if(req.ctx->isCancelled()) continue;

        if(errcode) sendError(req.id, errcode, errmsg);
        else        sendResult(req.id, result);
    }
}


QJsonObject Server::dispatch(QString const &method, QJsonObject const &params, Context &ctx)
{
    if(method=="initialize")                return onInitialize(params);
    if(method=="tools/list")                return onToolsList(params);
    if(method=="tools/call")                return onToolsCall(params, ctx);
    if(method=="resources/list")            return onResourcesList(params);
    if(method=="resources/templates/list")  return onResourceTemplatesList(params);
    if(method=="resources/read")            return onResourcesRead(params);
    if(method=="prompts/list")              return onPromptsList(params);
    if(method=="prompts/get")               return onPromptsGet(params);
    if(method=="completion/complete")       return onCompletion(params);
    if(method=="logging/setLevel")          return onSetLevel(params);

    throw RpcError(METHOD_NOT_FOUND, "Method not found: "+method);
}


QJsonObject Server::onInitialize(QJsonObject const &params)
{
    QString requested = params.value("protocolVersion").toString();
    if(supportedProtocolVersions().contains(requested)) m_ProtocolVersion = requested;
    else                                               m_ProtocolVersion = supportedProtocolVersions().front();

    QJsonObject caps;
    caps["tools"]       = QJsonObject{{"listChanged", false}};
    caps["resources"]   = QJsonObject{{"subscribe", false}, {"listChanged", true}};
    caps["prompts"]     = QJsonObject{{"listChanged", false}};
    caps["logging"]     = QJsonObject();
    caps["completions"] = QJsonObject();

    QJsonObject result;
    result["protocolVersion"] = m_ProtocolVersion;
    result["capabilities"] = caps;
    result["serverInfo"] = QJsonObject{{"name", m_Name}, {"title", "flow5"}, {"version", m_Version}};
    if(!m_Instructions.isEmpty()) result["instructions"] = m_Instructions;
    return result;
}


QJsonObject Server::onToolsList(QJsonObject const &) const
{
    QJsonArray tools;
    for(Tool const &t : m_Tools)
    {
        QJsonObject o;
        o["name"] = t.name;
        if(!t.title.isEmpty()) o["title"] = t.title;
        o["description"] = t.description;
        o["inputSchema"] = t.inputSchema;
        QJsonObject annotations = t.annotations;
        if(!t.title.isEmpty()) annotations["title"] = t.title;
        if(!annotations.isEmpty()) o["annotations"] = annotations;
        tools.append(o);
    }
    return QJsonObject{{"tools", tools}};
}


QJsonObject Server::onToolsCall(QJsonObject const &params, Context &ctx)
{
    QString name = params.value("name").toString();
    QJsonValue argsval = params.value("arguments");
    if(!argsval.isUndefined() && !argsval.isNull() && !argsval.isObject())
        throw RpcError(INVALID_PARAMS, "arguments must be an object");
    QJsonObject args = argsval.toObject();

    Tool const *pTool = nullptr;
    for(Tool const &t : m_Tools)
    {
        if(t.name==name) {pTool = &t; break;}
    }
    if(!pTool) throw RpcError(INVALID_PARAMS, "Unknown tool: "+name);

    QJsonObject result;
    try
    {
        ToolResult tr = pTool->handler(args, ctx);
        QJsonArray content;
        content.append(QJsonObject{{"type", "text"}, {"text", compact(tr.structured)}});
        if(!tr.text.isEmpty()) content.append(QJsonObject{{"type", "text"}, {"text", tr.text}});
        result["content"] = content;
        result["structuredContent"] = tr.structured;
        result["isError"] = false;
    }
    catch(std::exception const &e)
    {
        // ToolError and any other failure are reported to the model, not as protocol errors
        QJsonArray content;
        content.append(QJsonObject{{"type", "text"}, {"text", QString::fromStdString(e.what())}});
        result["content"] = content;
        result["isError"] = true;
        return result;
    }

    if(pTool->bMutates && m_bInitialized)
        notify("notifications/resources/list_changed", QJsonObject());

    return result;
}


QJsonObject Server::onResourcesList(QJsonObject const &) const
{
    QJsonArray resources;
    for(ResourceTemplate const &t : m_Templates)
    {
        if(!t.list) continue;
        for(ResourceInfo const &ri : t.list())
        {
            QJsonObject o;
            o["uri"] = ri.uri;
            o["name"] = ri.name;
            if(!ri.title.isEmpty())       o["title"] = ri.title;
            if(!ri.description.isEmpty()) o["description"] = ri.description;
            o["mimeType"] = ri.mimeType.isEmpty() ? t.mimeType : ri.mimeType;
            resources.append(o);
        }
    }
    return QJsonObject{{"resources", resources}};
}


QJsonObject Server::onResourceTemplatesList(QJsonObject const &) const
{
    QJsonArray templates;
    for(ResourceTemplate const &t : m_Templates)
    {
        if(!t.uriTemplate.contains('{')) continue; // fixed URIs are only listed by resources/list
        QJsonObject o;
        o["uriTemplate"] = t.uriTemplate;
        o["name"] = t.name;
        if(!t.title.isEmpty()) o["title"] = t.title;
        o["description"] = t.description;
        o["mimeType"] = t.mimeType;
        templates.append(o);
    }
    return QJsonObject{{"resourceTemplates", templates}};
}


bool Server::matchTemplate(ResourceTemplate const &tmpl, QString const &uri, std::map<QString, QString> &vars) const
{
    static QRegularExpression const varrx("\\{([A-Za-z_][A-Za-z0-9_]*)\\}");

    QString pattern = "^";
    QStringList names;
    int pos = 0;
    QRegularExpressionMatchIterator it = varrx.globalMatch(tmpl.uriTemplate);
    while(it.hasNext())
    {
        QRegularExpressionMatch m = it.next();
        pattern += QRegularExpression::escape(tmpl.uriTemplate.mid(pos, m.capturedStart()-pos));
        pattern += "([^/]+)";
        names.append(m.captured(1));
        pos = m.capturedEnd();
    }
    pattern += QRegularExpression::escape(tmpl.uriTemplate.mid(pos)) + "$";

    QRegularExpressionMatch m = QRegularExpression(pattern).match(uri);
    if(!m.hasMatch()) return false;

    vars.clear();
    for(int i=0; i<names.size(); i++)
        vars[names.at(i)] = QUrl::fromPercentEncoding(m.captured(i+1).toUtf8());
    return true;
}


QJsonObject Server::onResourcesRead(QJsonObject const &params) const
{
    QString uri = params.value("uri").toString();
    if(uri.isEmpty()) throw RpcError(INVALID_PARAMS, "Missing uri");

    for(ResourceTemplate const &t : m_Templates)
    {
        std::map<QString, QString> vars;
        if(!matchTemplate(t, uri, vars)) continue;

        ResourceContent rc;
        try
        {
            rc = t.read(vars);
        }
        catch(ToolError const &e)
        {
            throw RpcError(RESOURCE_NOT_FOUND, QString::fromStdString(e.what()));
        }

        QJsonObject content;
        content["uri"] = uri;
        content["mimeType"] = rc.mimeType.isEmpty() ? t.mimeType : rc.mimeType;
        content["text"] = rc.text;
        return QJsonObject{{"contents", QJsonArray{content}}};
    }

    throw RpcError(RESOURCE_NOT_FOUND, "Resource not found: "+uri);
}


QJsonObject Server::onPromptsList(QJsonObject const &) const
{
    QJsonArray prompts;
    for(Prompt const &p : m_Prompts)
    {
        QJsonObject o;
        o["name"] = p.name;
        if(!p.title.isEmpty()) o["title"] = p.title;
        o["description"] = p.description;
        QJsonArray args;
        for(PromptArgument const &a : p.arguments)
            args.append(QJsonObject{{"name", a.name}, {"description", a.description}, {"required", a.bRequired}});
        o["arguments"] = args;
        prompts.append(o);
    }
    return QJsonObject{{"prompts", prompts}};
}


QJsonObject Server::onPromptsGet(QJsonObject const &params) const
{
    QString name = params.value("name").toString();
    QJsonObject args = params.value("arguments").toObject();

    for(Prompt const &p : m_Prompts)
    {
        if(p.name!=name) continue;
        for(PromptArgument const &a : p.arguments)
        {
            if(a.bRequired && args.value(a.name).toString().isEmpty())
                throw RpcError(INVALID_PARAMS, "Missing required argument: "+a.name);
        }
        return QJsonObject{{"description", p.description}, {"messages", p.messages(args)}};
    }
    throw RpcError(INVALID_PARAMS, "Unknown prompt: "+name);
}


QJsonObject Server::onCompletion(QJsonObject const &params) const
{
    QJsonObject ref = params.value("ref").toObject();
    QJsonObject arg = params.value("argument").toObject();
    QString argname = arg.value("name").toString();
    QString prefix  = arg.value("value").toString();

    std::map<QString, QString> context;
    QJsonObject ctxargs = params.value("context").toObject().value("arguments").toObject();
    for(auto it=ctxargs.begin(); it!=ctxargs.end(); ++it) context[it.key()] = it.value().toString();

    QStringList values;
    QString type = ref.value("type").toString();
    if(type=="ref/prompt")
    {
        for(Prompt const &p : m_Prompts)
            if(p.name==ref.value("name").toString() && p.complete) values = p.complete(argname, prefix);
    }
    else if(type=="ref/resource")
    {
        for(ResourceTemplate const &t : m_Templates)
            if(t.uriTemplate==ref.value("uri").toString() && t.complete) values = t.complete(argname, prefix, context);
    }
    else throw RpcError(INVALID_PARAMS, "Invalid ref type: "+type);

    int total = int(values.size());
    if(values.size()>100) values = values.mid(0, 100);
    QJsonObject completion;
    completion["values"] = QJsonArray::fromStringList(values);
    completion["total"] = total;
    completion["hasMore"] = total>int(values.size());
    return QJsonObject{{"completion", completion}};
}


QJsonObject Server::onSetLevel(QJsonObject const &params)
{
    QString level = params.value("level").toString();
    for(int i=0; i<8; i++)
    {
        if(level==s_LevelNames[i])
        {
            m_LogLevel = i;
            return QJsonObject();
        }
    }
    throw RpcError(INVALID_PARAMS, "Invalid log level: "+level);
}


void Server::notify(QString const &method, QJsonObject const &params)
{
    QJsonObject msg{{"jsonrpc", "2.0"}, {"method", method}};
    if(!params.isEmpty()) msg["params"] = params;
    write(msg);
}


void Server::sendResult(QJsonValue const &id, QJsonObject const &result)
{
    write(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
}


void Server::sendError(QJsonValue const &id, int code, QString const &message)
{
    write(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"error", QJsonObject{{"code", code}, {"message", message}}}});
}


void Server::write(QJsonObject const &msg)
{
    QByteArray bytes = QJsonDocument(msg).toJson(QJsonDocument::Compact);
    std::lock_guard<std::mutex> lck(m_OutMutex);
    if(!m_pOut) return;
    fwrite(bytes.constData(), 1, size_t(bytes.size()), m_pOut);
    fputc('\n', m_pOut);
    fflush(m_pOut);
}
