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
 * Authors:
 *     Xavi Garcia <xavi.garcia.mena@canonical.com>
 *     Charles Kerr <charles.kerr@canonical.com>
 */

#include "helper/backup-helper.h"
#include "service/metadata.h"
#include "service/metadata-provider.h"
#include "service/keeper.h"
#include "storage-framework/storage_framework_client.h"

#include <QDebug>
#include <QDBusMessage>
#include <QDBusConnection>
#include <QStandardPaths>
#include <QStringList>
#include <QVariantMap>

#include <uuid/uuid.h>
#include "app-const.h"

class KeeperPrivate
{
public:

    Keeper * const q_ptr;
    QSharedPointer<MetadataProvider> backup_choices_;
    QSharedPointer<MetadataProvider> restore_choices_;
    QScopedPointer<BackupHelper> backup_helper_;
    QScopedPointer<StorageFrameworkClient> storage_;
    QVector<Metadata> cached_backup_choices_;
    QVector<Metadata> cached_restore_choices_;

    KeeperPrivate(Keeper* keeper,
                  const QSharedPointer<MetadataProvider>& backup_choices,
                  const QSharedPointer<MetadataProvider>& restore_choices)
        : q_ptr(keeper)
        , backup_choices_(backup_choices)
        , restore_choices_(restore_choices)
        , backup_helper_(new BackupHelper(DEKKO_APP_ID))
        , storage_(new StorageFrameworkClient())
        , cached_backup_choices_()
        , cached_restore_choices_()
    {
        QObject::connect(storage_.data(), &StorageFrameworkClient::socketReady, q_ptr, &Keeper::socketReady);
        QObject::connect(storage_.data(), &StorageFrameworkClient::socketClosed, q_ptr, &Keeper::socketClosed);

        // listen for backup helper state changes
        QObject::connect(backup_helper_.data(), &Helper::stateChanged,
            std::bind(&KeeperPrivate::on_backup_helper_state_changed, this, std::placeholders::_1)
        );
    }

    ~KeeperPrivate()
    {
    }

    Q_DISABLE_COPY(KeeperPrivate)

private:

    void on_backup_helper_state_changed(Helper::State state)
    {
        switch (state)
        {
            case Helper::State::NOT_STARTED:
                break;

            case Helper::State::STARTED:
                qDebug() << "Backup helper started";
                break;

            case Helper::State::CANCELLED:
            case Helper::State::FAILED:
            case Helper::State::COMPLETE:
                qDebug() << "Backup helper finished... closing the socket.";
                storage_->closeUploader();
                break;
        }
    }
};


Keeper::Keeper(const QSharedPointer<MetadataProvider>& backup_choices,
               const QSharedPointer<MetadataProvider>& restore_choices,
               QObject* parent)
    : QObject(parent)
    , d_ptr(new KeeperPrivate(this, backup_choices, restore_choices))
{
}

Keeper::~Keeper() = default;

void Keeper::start()
{
    Q_D(Keeper);

    qDebug() << "Backup start";
    qDebug() << "Waiting for a valid socket from the storage framework";

    d->storage_->getNewFileForBackup();
}

QDBusUnixFileDescriptor Keeper::StartBackup(quint64 /*nbytes*/)
{
    Q_D(Keeper);

    qDebug() << "Sending the socket " << d->backup_helper_->get_helper_socket();

    return QDBusUnixFileDescriptor(d->backup_helper_->get_helper_socket());
}

// FOR TESTING PURPOSES ONLY
// we should finish when the helper finishes
void Keeper::finish()
{
    Q_D(Keeper);

    qDebug() << "Closing the socket-------";

    d->storage_->closeUploader();
}

void Keeper::socketReady(int sd)
{
    Q_D(Keeper);

    qDebug() << "I've got a new socket: " << sd;
    qDebug() << "Starting the backup helper";
    d->backup_helper_->start(sd);
}

void Keeper::socketClosed()
{
    qDebug() << "The storage framework socket was closed";
}

QVector<Metadata>
Keeper::get_backup_choices()
{
    Q_D(Keeper);

    d->cached_backup_choices_ = d->backup_choices_->get_backups();
    return d->cached_backup_choices_;
}

QVector<Metadata>
Keeper::get_restore_choices()
{
    Q_D(Keeper);

    d->cached_restore_choices_ = d->restore_choices_->get_backups();
    return d->cached_restore_choices_;
}