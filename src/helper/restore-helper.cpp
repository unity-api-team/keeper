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

#include "util/connection-helper.h"
#include "helper/restore-helper.h"
#include "service/app-const.h" // HELPER_TYPE

#include <QByteArray>
#include <QDebug>
#include <QLocalSocket>
#include <QMap>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QCryptographicHash>
#include <QLocalServer>
#include <QSharedPointer>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <functional> // std::bind()


class RestoreHelperPrivate
{
public:

    explicit RestoreHelperPrivate(
        RestoreHelper* backup_helper
    )
        : q_ptr(backup_helper)
        , server_(new QLocalServer)
    {
        // listen for inactivity from storage framework
        QObject::connect(&timer_, &QTimer::timeout,
            std::bind(&RestoreHelperPrivate::on_inactivity_detected, this)
        );

        QObject::connect(&timer2_, &QTimer::timeout,
            std::bind(&RestoreHelperPrivate::delay_data, this)
        );

        // fire up the sockets
        int fds[2];
        int rc = socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds);
        if (rc == -1)
        {
            // TODO throw exception.
            qWarning() << "RestoreHelperPrivate: error creating socket pair to communicate with helper ";
            return;
        }

        // helper socket is for the client.
        helper_socket_.setSocketDescriptor(fds[1], QLocalSocket::ConnectedState, QIODevice::ReadOnly);

        write_socket_.setSocketDescriptor(fds[0], QLocalSocket::ConnectedState, QIODevice::WriteOnly);


        QObject::connect(server_.data(), &QLocalServer::newConnection, std::bind(&RestoreHelperPrivate::restore_ready, this));

        if (!server_->listen("test_helper"))
        {
            qWarning() << " GREP ERROR LISTENING";
        }
    }

    ~RestoreHelperPrivate() = default;

    Q_DISABLE_COPY(RestoreHelperPrivate)

    void start(QStringList const& urls)
    {
        q_ptr->Helper::start(urls);
        reset_inactivity_timer();
    }

    void set_downloader(std::shared_ptr<Downloader> const& downloader)
    {
        qDebug() << "RestoreHelper::set_downloader";
        downloader_ = downloader;
        n_read_ = 0;
        n_uploaded_ = 0;
        read_error_ = false;
        write_error_ = false;
        cancelled_ = false;

        q_ptr->set_expected_size(downloader->file_size());

        qDebug() << "Storage framework socket is: " << static_cast<void *>(downloader_->socket().get());
        // listen for data ready to read
        QObject::connect(downloader_->socket().get(), &QLocalSocket::readyRead,
            std::bind(&RestoreHelperPrivate::on_ready_read, this)
        );

        connections_.remember(QObject::connect(
            &write_socket_, &QLocalSocket::bytesWritten,
            std::bind(&RestoreHelperPrivate::on_data_uploaded, this, std::placeholders::_1)
        ));

        // maybe there's data already to be read
        process_more();

        reset_inactivity_timer();
    }

    void stop()
    {
        qDebug() << "GREP DISCONNECTING";
        write_socket_.disconnectFromServer();
        helper_conn_->disconnectFromServer();
        cancelled_ = true;
        q_ptr->Helper::stop();
    }

    int get_helper_socket() const
    {
        return int(helper_socket_.socketDescriptor());
    }

    QString to_string(Helper::State state) const
    {
        return state == Helper::State::STARTED
            ? QStringLiteral("restoring")
            : q_ptr->Helper::to_string(state);
    }

    void on_state_changed(Helper::State state)
    {
        switch (state)
        {
            case Helper::State::CANCELLED:
            case Helper::State::FAILED:
                qDebug() << "cancelled/failed, calling downloader_.reset()";
                downloader_.reset();
                break;

            case Helper::State::DATA_COMPLETE: {
                qDebug() << "Restore helper finished, calling downloader_.finish()";
                write_socket_.disconnectFromServer();
                helper_conn_->disconnectFromServer();
                downloader_->finish();
                downloader_.reset();
                break;
            }

            //case Helper::State::NOT_STARTED:
            //case Helper::State::STARTED:
            default:
                break;
        }
    }

    void on_helper_finished()
    {
        stop_inactivity_timer();
        check_for_done();
    }

    void restore_ready()
    {
        qDebug() << "GREP (((((((((((((((((((((((((((((((((((( HELPER IS READY: Starts sending data: TOTAL TO SEND: " << upload_buffer_.size();
        helper_is_ready_ = true;

        helper_conn_.reset(server_->nextPendingConnection());

        connections_.remember(QObject::connect(
            helper_conn_.data(), &QLocalSocket::bytesWritten,
            std::bind(&RestoreHelperPrivate::on_data_uploaded, this, std::placeholders::_1)
        ));
//        timer2_.start(10);
        send_data();
    }

private:

    void on_inactivity_detected()
    {
        stop_inactivity_timer();
        qWarning() << "Inactivity detected in the helper...stopping it";
        stop();
    }

    void send_data()
    {
        if (upload_buffer_.size())
        {
            int bytes_to_send = upload_buffer_.size() < UPLOAD_BUFFER_MAX_ ? upload_buffer_.size() : UPLOAD_BUFFER_MAX_;
            // try to empty the upload buf
            const auto n = helper_conn_->write(upload_buffer_, bytes_to_send);
            if (n > 0) {
                // THIS IS JUST FOR EXTRA DEBUG INFORMATION
                QCryptographicHash hash(QCryptographicHash::Sha1);
                hash.addData(upload_buffer_.left(100));
//                qDebug() << "GREP ************************************************ Hash send: " << hash.result().toHex() << " Size: " << n << " Total: " << upload_buffer_.size();
//                // THIS IS JUST FOR EXTRA DEBUG INFORMATION
                upload_buffer_.remove(0, int(n));
                qDebug() << "GREP ************************************************ Hash send: " << hash.result().toHex() << " Size: " << n << " Total: " << upload_buffer_.size();
                                // THIS IS JUST FOR EXTRA DEBUG INFORMATION
                waiting_for_data_to_be_written_ = true;
            }
            else {
                if (n < 0) {
                    write_error_ = true;
                    qWarning() << "Write error:" << write_socket_.errorString();
                    stop();
                }
            }
//            helper_conn_->flush();
//            timer2_.start(10);
        }
    }

    void on_ready_read()
    {
        process_more();
    }

    void on_data_uploaded(qint64 n)
    {
        waiting_for_data_to_be_written_ = false;
        n_uploaded_ += n;
        q_ptr->record_data_transferred(n);

        qDebug() << "GREP ====================================== Bytes uploaded: " << n << "Total: " << n_uploaded_;
        send_data();
//        qDebug("n_read %zu n_uploaded %zu (newly uploaded %zu)", size_t(n_read_), size_t(n_uploaded_), size_t(n));
//        process_more();
        check_for_done();
    }

    void delay_data()
    {
        send_data();
        //        qDebug("n_read %zu n_uploaded %zu (newly uploaded %zu)", size_t(n_read_), size_t(n_uploaded_), size_t(n));
        //        process_more();
        check_for_done();
    }

    void process_more()
    {
        if (!downloader_)
            return;
        char readbuf[UPLOAD_BUFFER_MAX_];
        auto socket = downloader_->socket();
        for(;;)
        {
            if (!socket->bytesAvailable())
                break;
            // try to fill the upload buf
//            int max_bytes = UPLOAD_BUFFER_MAX_ - upload_buffer_.size();
            int max_bytes = UPLOAD_BUFFER_MAX_;
            if (max_bytes > 0) {
                const auto n = socket->read(readbuf, max_bytes);
                if (n > 0) {
                    n_read_ += n;
                    upload_buffer_.append(readbuf, int(n));
                    QCryptographicHash hash(QCryptographicHash::Sha1);
                    hash.addData(readbuf, 100);
                    qDebug() << "GREP ########################### READ BYTES: " << n << "Total: " << n_read_ << " Hash: " << hash.result().toHex();
                    qDebug("upload_buffer_.size() is %zu after reading %zu from helper", size_t(upload_buffer_.size()), size_t(n));
                }
                else if (n < 0) {
                    read_error_ = true;
                    qDebug() << "Read error in restore helper: " << socket->errorString();
                    stop();
                    return;
                }
            }

            if (helper_is_ready_ && ! waiting_for_data_to_be_written_)
            {
                send_data();
            }

//            // THIS IS JUST FOR EXTRA DEBUG INFORMATION
//            QCryptographicHash hash(QCryptographicHash::Sha1);
//            hash.addData(upload_buffer_.left(100));
//            qDebug() << "************************************************ Hash send: " << hash.result().toHex() << " Size: " << upload_buffer_.size() << " Total: " << n_read_;
//            // THIS IS JUST FOR EXTRA DEBUG INFORMATION
//
//            // try to empty the upload buf
//            const auto n = write_socket_.write(upload_buffer_);
//            if (n > 0) {
//                upload_buffer_.remove(0, int(n));
//                qDebug("upload_buffer_.size() is %zu after writing %zu to cloud", size_t(upload_buffer_.size()), size_t(n));
//                continue;
//            }
//            else {
//                if (n < 0) {
//                    write_error_ = true;
//                    qWarning() << "Write error:" << write_socket_.errorString();
//                    stop();
//                }
//                break;
//            }
        }

        reset_inactivity_timer();
    }

    void reset_inactivity_timer()
    {
        static constexpr int MAX_TIME_WAITING_FOR_DATA {RestoreHelper::MAX_INACTIVITY_TIME};
        timer_.start(MAX_TIME_WAITING_FOR_DATA);
    }

    void stop_inactivity_timer()
    {
        timer_.stop();
    }

    void check_for_done()
    {
        qDebug() << "Checking for done.";
        qDebug() << "Expected: " << q_ptr->expected_size() << " Uploaded: " << n_uploaded_ << " Read: " << n_read_;
        if (cancelled_)
        {
            q_ptr->set_state(Helper::State::CANCELLED);
        }
        else if (read_error_ || write_error_ || n_uploaded_ > q_ptr->expected_size())
        {
            if (!q_ptr->is_helper_running())
            {
                q_ptr->set_state(Helper::State::FAILED);
            }
        }
        else if (n_uploaded_ == q_ptr->expected_size())
        {
            if (downloader_)
            {
                if (q_ptr->is_helper_running())
                {
                    // only in the case that the helper process finished we move to the next state
                    // this is to prevent to start the next task too early
                    q_ptr->set_state(Helper::State::DATA_COMPLETE);
                    stop_inactivity_timer();
                }
            }
            else
                q_ptr->set_state(Helper::State::COMPLETE);
        }
    }

    /***
    ****
    ***/

    static constexpr int UPLOAD_BUFFER_MAX_ {1024*16};

    RestoreHelper * const q_ptr;
    QTimer timer_;
    QTimer timer2_;
    std::shared_ptr<Downloader> downloader_;
    QLocalSocket helper_socket_;
    QLocalSocket write_socket_;
    QByteArray upload_buffer_;
    qint64 n_read_ = 0;
    qint64 n_uploaded_ = 0;
    bool read_error_ = false;
    bool write_error_ = false;
    bool cancelled_ = false;
    ConnectionHelper connections_;
    QString uploader_committed_file_name_;

    bool helper_is_ready_ = false;
    bool waiting_for_data_to_be_written_ = false;

    QSharedPointer<QLocalServer> server_;
    QSharedPointer<QLocalSocket> helper_conn_;
};

/***
****
***/

RestoreHelper::RestoreHelper(
    QString const & appid,
    clock_func const & clock,
    QObject * parent
)
    : Helper(appid, clock, parent)
    , d_ptr(new RestoreHelperPrivate(this))
{
}

RestoreHelper::~RestoreHelper() =default;

void
RestoreHelper::start(QStringList const& url)
{
    Q_D(RestoreHelper);

    d->start(url);
}

void
RestoreHelper::stop()
{
    Q_D(RestoreHelper);

    d->stop();
}

void
RestoreHelper::set_downloader(std::shared_ptr<Downloader> const& downloader)
{
    Q_D(RestoreHelper);

    d->set_downloader(downloader);
}

int
RestoreHelper::get_helper_socket() const
{
    Q_D(const RestoreHelper);

    return d->get_helper_socket();
}

QString
RestoreHelper::to_string(Helper::State state) const
{
    Q_D(const RestoreHelper);

    return d->to_string(state);
}

void
RestoreHelper::set_state(Helper::State state)
{
    Q_D(RestoreHelper);

    qDebug() << Q_FUNC_INFO;
    Helper::set_state(state);
    d->on_state_changed(state);
}

void RestoreHelper::on_helper_finished()
{
    Q_D(RestoreHelper);

    Helper::on_helper_finished();
    d->on_helper_finished();
}

void RestoreHelper::restore_ready()
{
    Q_D(RestoreHelper);

    d->restore_ready();
}