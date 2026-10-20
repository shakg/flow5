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

#pragma once

/**
 * @file Minimal, dependency-free (QtCore only) implementation of a
 * Model Context Protocol server over stdio.
 * Transport: newline-delimited JSON-RPC 2.0 messages on stdin/stdout.
 *
 * Threading model:
 * - the reader thread parses stdin, answers ping, and handles notifications
 *   (initialized, cancelled) immediately;
 * - all other requests are executed sequentially on a single worker thread,
 *   since the flow5 object arrays are global and not thread-safe.
 */

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace mcp
{
    /** JSON-RPC error codes */
    enum enumErrorCode
    {
        PARSE_ERROR      = -32700,
        INVALID_REQUEST  = -32600,
        METHOD_NOT_FOUND = -32601,
        INVALID_PARAMS   = -32602,
        INTERNAL_ERROR   = -32603,
        RESOURCE_NOT_FOUND = -32002
    };

    /** Thrown by handlers to return a JSON-RPC protocol error */
    class RpcError : public std::runtime_error
    {
        public:
            RpcError(int code, QString const &msg) : std::runtime_error(msg.toStdString()), m_Code(code) {}
            int code() const {return m_Code;}
        private:
            int m_Code;
    };

    /** Thrown by tool handlers to report a tool execution error, i.e. a result with isError=true */
    class ToolError : public std::runtime_error
    {
        public:
            explicit ToolError(QString const &msg) : std::runtime_error(msg.toStdString()) {}
    };

    enum enumLogLevel {DEBUG, INFO, NOTICE, WARNING, ERROR, CRITICAL, ALERT, EMERGENCY};

    class Server;

    /** Per-request context passed to the handlers */
    class Context
    {
        friend class Server;
        public:
            /** Sends a notifications/progress message if the client provided a progress token */
            void progress(double done, double total, QString const &message=QString());
            /** Sends a notifications/message log message, filtered by the level set by the client */
            void log(enumLogLevel level, QString const &message);
            bool isCancelled() const {return m_bCancelled.load();}
            /** Registers a function called from the reader thread when the client cancels the request */
            void setCancelHook(std::function<void()> hook);

        private:
            Server *m_pServer = nullptr;
            QJsonValue m_ProgressToken;
            std::atomic<bool> m_bCancelled{false};
            std::mutex m_HookMutex;
            std::function<void()> m_CancelHook;
    };


    /** Installs a cancel hook for the lifetime of the scope */
    class CancelHookScope
    {
        public:
            CancelHookScope(Context &ctx, std::function<void()> hook) : m_Ctx(ctx) {m_Ctx.setCancelHook(std::move(hook));}
            ~CancelHookScope() {m_Ctx.setCancelHook(nullptr);}
            CancelHookScope(CancelHookScope const &) = delete;
            CancelHookScope &operator=(CancelHookScope const &) = delete;
        private:
            Context &m_Ctx;
    };


    /** A tool's output. Sent as structuredContent and as a text block containing the same JSON, plus optional extra text. */
    struct ToolResult
    {
        QJsonObject structured;
        QString text;          /**< optional human-readable text appended as a second content block */
    };

    struct Tool
    {
        QString name;
        QString title;
        QString description;
        QJsonObject inputSchema;
        QJsonObject annotations;
        bool bMutates = false; /**< if true, a notifications/resources/list_changed is sent after a successful call */
        std::function<ToolResult(QJsonObject const &args, Context &ctx)> handler;
    };

    struct ResourceContent
    {
        QString mimeType;
        QString text;
    };

    struct ResourceInfo
    {
        QString uri;
        QString name;
        QString title;
        QString description;
        QString mimeType;
    };

    /** A resource template, e.g. flow5://foil/{name}.
     * The template is matched by converting each {var} into a capture group. */
    struct ResourceTemplate
    {
        QString uriTemplate;
        QString name;
        QString title;
        QString description;
        QString mimeType;
        std::function<ResourceContent(std::map<QString, QString> const &vars)> read;
        /** returns the concrete resources currently available for this template */
        std::function<std::vector<ResourceInfo>()> list;
        /** returns completion candidates for a template variable */
        std::function<QStringList(QString const &var, QString const &prefix, std::map<QString, QString> const &context)> complete;
    };

    struct PromptArgument
    {
        QString name;
        QString description;
        bool bRequired = false;
    };

    struct Prompt
    {
        QString name;
        QString title;
        QString description;
        std::vector<PromptArgument> arguments;
        std::function<QJsonArray(QJsonObject const &args)> messages;
        std::function<QStringList(QString const &arg, QString const &prefix)> complete;
    };


    class Server
    {
        friend class Context;
        public:
            Server(QString const &name, QString const &version, QString const &instructions);

            void addTool(Tool const &tool);
            void addResourceTemplate(ResourceTemplate const &tmpl);
            void addPrompt(Prompt const &prompt);

            /** Runs until stdin is closed. outFd is the file descriptor reserved for protocol output. */
            int run(int outFd);

            /** Sends a notification to the client; thread safe */
            void notify(QString const &method, QJsonObject const &params);

            static QStringList supportedProtocolVersions();

        private:
            struct PendingRequest
            {
                QJsonValue id;
                QString method;
                QJsonObject params;
                std::shared_ptr<Context> ctx;
            };

            void readerLoop();
            void workerLoop();

            void handleMessage(QJsonObject const &msg);
            void handleNotification(QString const &method, QJsonObject const &params);
            QJsonObject dispatch(QString const &method, QJsonObject const &params, Context &ctx);

            QJsonObject onInitialize(QJsonObject const &params);
            QJsonObject onToolsList(QJsonObject const &params) const;
            QJsonObject onToolsCall(QJsonObject const &params, Context &ctx);
            QJsonObject onResourcesList(QJsonObject const &params) const;
            QJsonObject onResourceTemplatesList(QJsonObject const &params) const;
            QJsonObject onResourcesRead(QJsonObject const &params) const;
            QJsonObject onPromptsList(QJsonObject const &params) const;
            QJsonObject onPromptsGet(QJsonObject const &params) const;
            QJsonObject onCompletion(QJsonObject const &params) const;
            QJsonObject onSetLevel(QJsonObject const &params);

            bool matchTemplate(ResourceTemplate const &tmpl, QString const &uri, std::map<QString, QString> &vars) const;

            void sendResult(QJsonValue const &id, QJsonObject const &result);
            void sendError(QJsonValue const &id, int code, QString const &message);
            void write(QJsonObject const &msg);

        private:
            QString m_Name, m_Version, m_Instructions;
            QString m_ProtocolVersion;

            std::vector<Tool> m_Tools;
            std::vector<ResourceTemplate> m_Templates;
            std::vector<Prompt> m_Prompts;

            std::atomic<int> m_LogLevel{INFO};
            std::atomic<bool> m_bInitialized{false};
            std::atomic<bool> m_bStop{false};

            FILE *m_pOut = nullptr;
            std::mutex m_OutMutex;

            std::mutex m_QueueMutex;
            std::condition_variable m_QueueCv;
            std::deque<PendingRequest> m_Queue;

            std::mutex m_ActiveMutex;
            std::map<QString, std::shared_ptr<Context>> m_Active; /**< requests queued or running, keyed by serialized id */
    };

    /** Helpers to read typed tool arguments; throw ToolError on missing or malformed values */
    QString     argString(QJsonObject const &args, QString const &key);
    QString     argString(QJsonObject const &args, QString const &key, QString const &defaultValue);
    double      argDouble(QJsonObject const &args, QString const &key);
    double      argDouble(QJsonObject const &args, QString const &key, double defaultValue);
    int         argInt(QJsonObject const &args, QString const &key);
    int         argInt(QJsonObject const &args, QString const &key, int defaultValue);
    bool        argBool(QJsonObject const &args, QString const &key, bool defaultValue);
    QJsonArray  argArray(QJsonObject const &args, QString const &key);

    /** Parses a JSON literal; used to write tool schemas inline. Aborts on malformed input since that is a programming error. */
    QJsonObject json(char const *literal);

    inline QString qs(std::string const &s) {return QString::fromStdString(s);}
}
