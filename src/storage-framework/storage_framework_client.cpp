/*
 * Copyright (C) 2016 Canonical, Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranties of
 * MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Xavi Garcia <xavi.garcia.mena@canonical.com>
 */
#include <QLocalSocket>
#include "storage_framework_client.h"


#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

using namespace unity::storage::qt::client;

StorageFrameworkClient::StorageFrameworkClient(QObject *parent)
    : QObject(parent)
    , runtime_(Runtime::create())
    , uploader_ready_watcher_(parent)
    , uploader_closed_watcher_(parent)
    , uploader_()
{
    QObject::connect(&uploader_ready_watcher_,&QFutureWatcher<std::shared_ptr<Uploader>>::finished, this, &StorageFrameworkClient::uploaderReady);
    QObject::connect(&uploader_closed_watcher_,&QFutureWatcher<std::shared_ptr<File>>::finished, this, &StorageFrameworkClient::onUploaderClosed);
    QObject::connect(&accounts_watcher_,&QFutureWatcher<QVector<std::shared_ptr<Account>>>::finished, this, &StorageFrameworkClient::accountsReady);
}

void StorageFrameworkClient::getNewFileForBackup(quint64 n_bytes)
{
    accounts_watcher_.setProperty("n_bytes", n_bytes);
    accounts_watcher_.setFuture(runtime_->accounts());
}

void StorageFrameworkClient::finish(bool do_commit)
{

    if (!uploader_ || !do_commit)
    {
        qDebug() << "StorageFrameworkClient::finish() is throwing away the file";
        uploader_.reset();
        Q_EMIT(finished());
    }
    else try
    {
        qDebug() << "StorageFrameworkClient::finish() is committing";
        uploader_closed_watcher_.setFuture(uploader_->finish_upload());
    }
    catch (std::exception & e)
    {
        qDebug() << "ERROR: StorageFrameworkClient::closeUploader():" << e.what();
        throw;
    }
}

void StorageFrameworkClient::onUploaderClosed()
{
    auto file = uploader_closed_watcher_.result();
    qDebug() << "Uploader for file" << file->name() << "was closed";
    qDebug() << "Uploader was closed";
    uploader_->socket()->disconnectFromServer();
    uploader_.reset();
    Q_EMIT(finished());
}

void StorageFrameworkClient::accountsReady()
{
    // Get the acccounts. (There is only one account for the client implementation.)
    try {
//        auto accounts = accounts_watcher_.result();

//        if (accounts.empty())
//        {
//            throw std::runtime_error("No accounts returned from Storage Framework");
//        }

//        Root::SPtr root = accounts[0]->roots().result()[0];
        Account::SPtr test_acc;
        int acc_id = 0;
        while (!test_acc)
        {
            try
            {
                test_acc = runtime_->make_test_account("com.canonical.StorageFramework.Provider.McloudProvider", "/provider/" + acc_id++);
            }
            catch (...) {}
        }

        Root::SPtr root = test_acc->roots().result()[0];

        qDebug() << "id:" << root->native_identity();
        qDebug() << "time:" << root->last_modified_time();

        // XGM ADD A new file to the root
        QFutureWatcher<std::shared_ptr<Uploader>> new_file_watcher;

        // get the current date and time to create the new file
        QDateTime now = QDateTime::currentDateTime();
        QString new_file_name = QString("Backup_%1").arg(now.toString("dd.MM.yyyy-hh:mm:ss.zzz"));

        uploader_ready_watcher_.setFuture(root->create_file(new_file_name, accounts_watcher_.property("n_bytes").toUInt()));
    }
    catch (std::exception & e)
    {
        qDebug() << "ERROR: StorageFrameworkClient::getNewFileForBackup():" << e.what();
        throw;
    }
}

void StorageFrameworkClient::uploaderReady()
{
    uploader_ = uploader_ready_watcher_.result();

    Q_EMIT (socketReady(uploader_->socket()));
}
