/*
 * This file is part of the PulseView project.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2014 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */
  

#include <stdexcept>
#include <algorithm>

#include <QDebug>

#include "decoderstack.h"

#include "logic.h"
#include "logicsnapshot.h"
#include "decode/decoder.h"
#include "decode/annotation.h"
#include "decode/rowdata.h"
#include "../sigsession.h"
#include "../view/logicsignal.h"
#include "../dsvdef.h"


using namespace boost;
using namespace std;

using namespace pv::data::decode;

namespace pv {
namespace data {

const double DecoderStack::DecodeMargin = 1.0;
const double DecoderStack::DecodeThreshold = 0.2;
const int64_t DecoderStack::DecodeChunkLength = 4 * 1024;
//const int64_t DecoderStack::DecodeChunkLength = 1024 * 1024;
const unsigned int DecoderStack::DecodeNotifyPeriod = 1024;

std::mutex DecoderStack::_global_decode_mutex;

DecoderStack::DecoderStack(pv::SigSession *session,
	const srd_decoder *const dec, DecoderStatus *decoder_status) :
	_session(session)
{
    _samples_decoded = 0;
    _sample_count = 0;
    _frame_complete = false;
    _decode_state = Stopped;
    _options_changed = false;
    _no_memory = false;
    _mark_index = -1;
    _decoder_status = decoder_status;
    _bThreadStop = false;
    _decode_thread = NULL;

	connect(_session, SIGNAL(frame_began()),
		this, SLOT(on_new_frame()));
	connect(_session, SIGNAL(data_received()),
		this, SLOT(on_data_received()));
	connect(_session, SIGNAL(frame_ended()),
		this, SLOT(on_frame_ended()));

    _stack.push_back(boost::shared_ptr<decode::Decoder>(
		new decode::Decoder(dec)));
 
    build_row();
}

DecoderStack::~DecoderStack()
{ 
    stop_decode();

    for (auto &kv : _rows)
    {
        delete kv.second;
    }
    _rows.clear();

    _stack.clear();
    _rows_gshow.clear();
    _rows_lshow.clear();
    _class_rows.clear();
}
 
void DecoderStack::push(boost::shared_ptr<decode::Decoder> decoder)
{
	assert(decoder);
	_stack.push_back(decoder);
    build_row();
    _options_changed = true;
}

void DecoderStack::remove(boost::shared_ptr<Decoder> &decoder)
{
	// Find the decoder in the stack
    list< boost::shared_ptr<Decoder> >::iterator iter = _stack.begin();
    for(unsigned int i = 0; i < _stack.size(); i++, iter++)
        if ((*iter) == decoder)
            break;

	// Delete the element
    if (iter != _stack.end())
        _stack.erase(iter);

    build_row();
    _options_changed = true;
}

void DecoderStack::build_row()
{
    //destory data
    for (auto &kv : _rows)
    {
        delete kv.second;
    }
    _rows.clear();

    // Add classes
    for (auto &dec : _stack)
    {
        assert(dec);
        const srd_decoder *const decc = dec->decoder();
        assert(dec->decoder());

        // Add a row for the decoder if it doesn't have a row list
        if (!decc->annotation_rows) {
            const Row row(decc);
            _rows[row] = new decode::RowData();
            std::map<const decode::Row, bool>::const_iterator iter = _rows_gshow.find(row);
            if (iter == _rows_gshow.end()) {
                _rows_gshow[row] = true;
                if (row.title().contains("bit", Qt::CaseInsensitive) ||
                    row.title().contains("warning", Qt::CaseInsensitive)) {
                    _rows_lshow[row] = false;
                } else {
                    _rows_lshow[row] = true;
                }
            }
        }

        // Add the decoder rows
        int order = 0;
        for (const GSList *l = decc->annotation_rows; l; l = l->next)
        {
            const srd_decoder_annotation_row *const ann_row =
                (srd_decoder_annotation_row *)l->data;
            assert(ann_row);

            const Row row(decc, ann_row, order);

            // Add a new empty row data object
            _rows[row] = new decode::RowData();
            std::map<const decode::Row, bool>::const_iterator iter = _rows_gshow.find(row);
            if (iter == _rows_gshow.end()) {
                _rows_gshow[row] = true;
                if (row.title().contains("bit", Qt::CaseInsensitive) ||
                    row.title().contains("warning", Qt::CaseInsensitive)) {
                    _rows_lshow[row] = false;
                } else {
                    _rows_lshow[row] = true;
                }
            }

            // Map out all the classes
            for (const GSList *ll = ann_row->ann_classes;
                ll; ll = ll->next)
                _class_rows[make_pair(decc,
                    GPOINTER_TO_INT(ll->data))] = row;

            order++;
        }
    }
}

int64_t DecoderStack::samples_decoded() const
{
    std::lock_guard<std::mutex> decode_lock(_output_mutex);
	return _samples_decoded;
}

void DecoderStack::get_annotation_subset(
	std::vector<pv::data::decode::Annotation> &dest,
	const Row &row, uint64_t start_sample,
	uint64_t end_sample) const
{  
    auto iter = _rows.find(row);
    if (iter != _rows.end())
        (*iter).second->get_annotation_subset(dest,
			start_sample, end_sample);
}


uint64_t DecoderStack::get_annotation_index(
    const Row &row, uint64_t start_sample) const
{  
    uint64_t index = 0;
    auto iter = _rows.find(row);
    if (iter != _rows.end())
        index = (*iter).second->get_annotation_index(start_sample);

    return index;
}

uint64_t DecoderStack::get_max_annotation(const Row &row)
{ 
    auto iter =  _rows.find(row);
    if (iter != _rows.end())
        return (*iter).second->get_max_annotation();

    return 0;
}

uint64_t DecoderStack::get_min_annotation(const Row &row)
{  
    auto iter = _rows.find(row);
    if (iter != _rows.end())
        return (*iter).second->get_min_annotation();

    return 0;
}

std::map<const decode::Row, bool> DecoderStack::get_rows_gshow()
{
    std::map<const decode::Row, bool> rows_gshow;
    for (std::map<const decode::Row, bool>::const_iterator i = _rows_gshow.begin();
        i != _rows_gshow.end(); i++) {
        rows_gshow[(*i).first] = (*i).second;
    }
    return rows_gshow;
}

std::map<const decode::Row, bool> DecoderStack::get_rows_lshow()
{
    std::map<const decode::Row, bool> rows_lshow;
    for (std::map<const decode::Row, bool>::const_iterator i = _rows_lshow.begin();
        i != _rows_lshow.end(); i++) {
        rows_lshow[(*i).first] = (*i).second;
    }
    return rows_lshow;
}

void DecoderStack::set_rows_gshow(const decode::Row row, bool show)
{
    std::map<const decode::Row, bool>::const_iterator iter = _rows_gshow.find(row);
    if (iter != _rows_gshow.end()) {
        _rows_gshow[row] = show;
    }
}

void DecoderStack::set_rows_lshow(const decode::Row row, bool show)
{
    std::map<const decode::Row, bool>::const_iterator iter = _rows_lshow.find(row);
    if (iter != _rows_lshow.end()) {
        _rows_lshow[row] = show;
    }
}

bool DecoderStack::has_annotations(const Row &row) const
{  
    auto iter =
        _rows.find(row);
    if (iter != _rows.end())
        if(0 == (*iter).second->get_max_sample())
            return false;
        else
            return true;
    else
        return false;
}

uint64_t DecoderStack::list_annotation_size() const
{
    std::lock_guard<std::mutex> lock(_output_mutex);
    uint64_t max_annotation_size = 0;
    for (auto i = _rows.begin();
        i != _rows.end(); i++) {
        map<const Row, bool>::const_iterator iter = _rows_lshow.find((*i).first);
        if (iter != _rows_lshow.end() && (*iter).second)
            max_annotation_size = max(max_annotation_size,
                (*i).second->get_annotation_size());
    }

    return max_annotation_size;
}

uint64_t DecoderStack::list_annotation_size(uint16_t row_index) const
{ 
    for (auto i = _rows.begin();
        i != _rows.end(); i++) {
        map<const Row, bool>::const_iterator iter = _rows_lshow.find((*i).first);
        if (iter != _rows_lshow.end() && (*iter).second)
            if (row_index-- == 0) {
                return (*i).second->get_annotation_size();
            }
    }
    return 0;
}

bool DecoderStack::list_annotation(pv::data::decode::Annotation &ann,
                                  uint16_t row_index, uint64_t col_index) const
{ 
    for (auto i = _rows.begin();
        i != _rows.end(); i++) {
        map<const Row, bool>::const_iterator iter = _rows_lshow.find((*i).first);
        if (iter != _rows_lshow.end() && (*iter).second) {
            if (row_index-- == 0) {
                return (*i).second->get_annotation(ann, col_index);
            }
        }
    }

    return false;
}


bool DecoderStack::list_row_title(int row, QString &title) const
{ 
    for (auto i = _rows.begin();
        i != _rows.end(); i++) {
        map<const Row, bool>::const_iterator iter = _rows_lshow.find((*i).first);
        if (iter != _rows_lshow.end() && (*iter).second) {
            if (row-- == 0) {
                title = (*i).first.title();
                return 1;
            }
        }
    }
    return 0;
}

QString DecoderStack::error_message()
{ 
	return _error_message;
}

void DecoderStack::clear()
{
    init();
}

void DecoderStack::init()
{
    _sample_count = 0;
    _frame_complete = false;
    _samples_decoded = 0;
    _error_message = QString();
    _no_memory = false;

    for (auto i = _rows.begin();
        i != _rows.end(); i++) { 
        (*i).second->clear();
    }

    set_mark_index(-1);
}

void DecoderStack::stop_decode()
{ 
    _bThreadStop = true;

     if (_decode_thread && _decode_thread->joinable()) { 
        _decode_thread->join(); 
     }
     DESTROY_OBJECT(_decode_thread);

    if(_decode_state != Stopped) {       
       _decode_state = Stopped;
    }
}

void DecoderStack::begin_decode()
{
    boost::shared_ptr<pv::view::LogicSignal> logic_signal;
    boost::shared_ptr<pv::data::Logic> data;

    if (!_options_changed)
        return;
    _options_changed = false;
    stop_decode();
    init();

	// Check that all decoders have the required channels
    for(auto &dec : _stack)
		if (!dec->have_required_probes()) {
			_error_message = tr("One or more required channels "
				"have not been specified");
			return;
		}

	// We get the logic data of the first channel in the list.
	// This works because we are currently assuming all
	// LogicSignals have the same data/snapshot
    for (auto &dec : _stack) {
        if (dec && !dec->channels().empty()) {
            for(auto &sig :  _session->get_signals()) {
                if((sig->get_index() == (*dec->channels().begin()).second) &&
                   (logic_signal = dynamic_pointer_cast<view::LogicSignal>(sig)) &&
                   (data = logic_signal->logic_data()))
                    break;
            }
            if (data)
                break;
        }
    }

	if (!data)
		return;

	// Check we have a snapshot of data
    const deque< boost::shared_ptr<pv::data::LogicSnapshot> > &snapshots =
		data->get_snapshots();
	if (snapshots.empty())
		return;
	_snapshot = snapshots.front();
    if (_snapshot->empty())
        return;

    // Get the samplerate
	_samplerate = data->samplerate();
    if (_samplerate == 0.0)
        return;

    if (_decode_thread && _decode_thread->joinable()) { 
            _bThreadStop = true;
            _decode_thread->join(); 
    }
    DESTROY_OBJECT(_decode_thread);

    _bThreadStop = false; //reset stop flag
    _decode_thread =  new std::thread(&DecoderStack::decode_proc, this);
}

uint64_t DecoderStack::get_max_sample_count() const
{
	uint64_t max_sample_count = 0;

    for (auto i = _rows.begin(); i != _rows.end(); i++){
        max_sample_count = max(max_sample_count, (*i).second->get_max_sample());
    } 	

	return max_sample_count;
}

void DecoderStack::decode_data(
    const uint64_t decode_start, const uint64_t decode_end,
    srd_session *const session)
{
    //uint8_t *chunk = NULL;
    uint64_t last_cnt = 0;
    uint64_t notify_cnt = (decode_end - decode_start + 1)/100;
    srd_decoder_inst *logic_di = NULL;
    // find the first level decoder instant
    for (GSList *d = session->di_list; d; d = d->next) {
        srd_decoder_inst *di = (srd_decoder_inst *)d->data;
        srd_decoder *decoder = di->decoder;
        const bool have_probes = (decoder->channels || decoder->opt_channels) != 0;
        if (have_probes) {
            logic_di = di;
            break;
        }
    }

    uint64_t entry_cnt = 0;
    uint64_t i = decode_start;
    char *error = NULL;

    while(!_bThreadStop && i < decode_end && !_no_memory)
    { 
        std::vector<const uint8_t *> chunk;
        std::vector<uint8_t> chunk_const;
        uint64_t chunk_end = decode_end;

        for (int j =0 ; j < logic_di->dec_num_channels; j++) {
            int sig_index = logic_di->dec_channelmap[j];
            if (sig_index == -1) {
                chunk.push_back(NULL);
                chunk_const.push_back(0);
            } else {
                if (_snapshot->has_data(sig_index)) {
                    chunk.push_back(_snapshot->get_samples(i, chunk_end, sig_index));
                    chunk_const.push_back(_snapshot->get_sample(i, sig_index));
                } else {
                    _error_message = tr("At least one of selected channels are not enabled.");
                    return;
                }
            }
        }
        if (chunk_end > decode_end)
            chunk_end = decode_end;
        if (chunk_end - i > MaxChunkSize)
            chunk_end = i + MaxChunkSize;

        if (srd_session_send(session, i, chunk_end,
                             chunk.data(), chunk_const.data(), chunk_end - i, &error) != SRD_OK) {
            _error_message = QString::fromLocal8Bit(error);
            break;
        }
        i = chunk_end;

        {
            std::lock_guard<std::mutex> lock(_output_mutex);
            _samples_decoded = i - decode_start + 1;
        }

        if ((i - last_cnt) > notify_cnt) {
            last_cnt = i;
            new_decode_data();
        }
        entry_cnt++;
    }
    if (error)
        g_free(error);
    decode_done();
}

void DecoderStack::decode_proc()
{ 
    std::lock_guard<std::mutex> decode_lock(_global_decode_mutex);
    
    if (_bThreadStop){
        return;
    }

    optional<uint64_t> sample_count;
	srd_session *session;
	srd_decoder_inst *prev_di = NULL;
    uint64_t decode_start = 0;
    uint64_t decode_end = 0;

	assert(_snapshot);

	// Create the session
	srd_session_new(&session);
	assert(session);

    _decode_state = Running;

    // Get the intial sample count
    sample_count = _sample_count = _snapshot->get_sample_count();

    // Create the decoders
    for(auto &dec : _stack)
	{
        srd_decoder_inst *const di = dec->create_decoder_inst(session);

		if (!di)
		{
			_error_message = tr("Failed to create decoder instance");
			srd_session_destroy(session);
			return;
		}

		if (prev_di)
			srd_inst_stack (session, prev_di, di);

		prev_di = di;
        decode_start = dec->decode_start();
        decode_end = min(dec->decode_end(), _sample_count-1);
	}

	// Start the session
	srd_session_metadata_set(session, SRD_CONF_SAMPLERATE,
		g_variant_new_uint64((uint64_t)_samplerate));

	srd_pd_output_callback_add(session, SRD_OUTPUT_ANN,
		DecoderStack::annotation_callback, this);

    char *error = NULL;
    if (srd_session_start(session, &error) == SRD_OK)
       //need a lot time
        decode_data(decode_start, decode_end, session);
    else
        _error_message = QString::fromLocal8Bit(error);

	// Destroy the session
    if (error) {
        g_free(error);
    }
	srd_session_destroy(session);

    _decode_state = Stopped;
}

uint64_t DecoderStack::sample_count() const
{
    if (_snapshot)
        return _snapshot->get_sample_count();
    else
        return 0;
}

uint64_t DecoderStack::sample_rate() const
{
    return _samplerate;
}

//the decode callback, annotation object will be create
void DecoderStack::annotation_callback(srd_proto_data *pdata, void *decoder)
{
	assert(pdata);
	assert(decoder);

	DecoderStack *const d = (DecoderStack*)decoder;
	assert(d);
  
    if (d->_no_memory) {
        return;
    }

    Annotation *a = new Annotation(pdata, d->_decoder_status);
    if (a == NULL){
        d->_no_memory = true;
        return;     
    }

	// Find the row
	assert(pdata->pdo);
	assert(pdata->pdo->di);
	const srd_decoder *const decc = pdata->pdo->di->decoder;
	assert(decc);

    auto row_iter = d->_rows.end();
	
	// Try looking up the sub-row of this class
	const map<pair<const srd_decoder*, int>, Row>::const_iterator r =
        d->_class_rows.find(make_pair(decc, a->format()));
	if (r != d->_class_rows.end())
        row_iter = d->_rows.find((*r).second);
	else
	{
		// Failing that, use the decoder as a key
        row_iter = d->_rows.find(Row(decc));
	}

    assert(row_iter != d->_rows.end());
    if (row_iter == d->_rows.end()) {
        qDebug() << "Unexpected annotation: decoder = " << decc <<
            ", format = " << a->format();
        assert(0);
        return;
    }

	// Add the annotation 
    if (!(*row_iter).second->push_annotation(a))
        d->_no_memory = true; 
}

void DecoderStack::on_new_frame()
{
    //begin_decode();
}

void DecoderStack::on_data_received()
{  
}

void DecoderStack::on_frame_ended()
{ 
    _options_changed = true;
    begin_decode();
}

int DecoderStack::list_rows_size()
{ 
    int rows_size = 0;
    for (auto i = _rows.begin(); i != _rows.end(); i++) {
        map<const Row, bool>::const_iterator iter = _rows_lshow.find((*i).first);
        if (iter != _rows_lshow.end() && (*iter).second)
            rows_size++;
    }
    return rows_size;
}

bool DecoderStack::options_changed() const
{
    return _options_changed;
}

void DecoderStack::set_options_changed(bool changed)
{
    _options_changed = changed;
}

bool DecoderStack::out_of_memory() const
{
    return _no_memory;
}

void DecoderStack::set_mark_index(int64_t index)
{
    _mark_index = index;
}

int64_t DecoderStack::get_mark_index() const
{
    return _mark_index;
}

} // namespace data
} // namespace pv
