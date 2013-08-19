/*
 * Copyright (C) 2013 Andreas Steffen
 * HSR Hochschule fuer Technik Rapperswil
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include "imc_swid_state.h"

#include "libpts.h"
#include "swid/swid_tag.h"
#include "swid/swid_tag_id.h"
#include "tcg/swid/tcg_swid_attr_req.h"
#include "tcg/swid/tcg_swid_attr_tag_inv.h"
#include "tcg/swid/tcg_swid_attr_tag_id_inv.h"

#include <imc/imc_agent.h>
#include <imc/imc_msg.h>

#include <tncif_pa_subtypes.h>

#include <pen/pen.h>
#include <utils/debug.h>

static char strongswan_tag[] =
	"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
	"<software_identification_tag "
	"xmlns=\"http://standards.iso.org/iso/19770/-2/2009/schema.xsd\">\n"
	"<entitlement_required_indicator>true</entitlement_required_indicator>\n"
	"<product_title>strongSwan</product_title>\n"
	"<product_version>\n"
	"  <name>5.1.1dr1</name>\n"
	"  <numeric>\n"
	"    <major>5</major>\n"
	"    <minor>1</minor>\n"
	"    <build>1</build>\n"
	"    <review>dr1</review>\n"
	"  </numeric>\n"
	"</product_version>\n"
	"<software_creator>\n"
	"  <name>strongSwan Project</name>\n"
	"  <regid>regid.2004-03.org.strongswan</regid>\n"
	"</software_creator>\n"
	"<software_licensor>\n"
	"  <name>strongSwan Project</name>\n"
	"  <regid>regid.2004-03.org.strongswan</regid>\n"
	"</software_licensor>\n"
	"<software_id>\n"
	"  <unique_id>strongSwan-5-1-1-dr1</unique_id>\n"
	"  <tag_creator_regid>regid.2004-03.org.strongswan</tag_creator_regid>\n"
	"</software_id>\n"
	"<tag_creator>\n"
	"  <name>strongSwan Project</name>\n"
	"  <regid>regid.2004-03.org.strongswan</regid>\n"
	"</tag_creator>\n"
	"</software_identification_tag>\n";

/* IMC definitions */

static const char imc_name[] = "SWID";

static pen_type_t msg_types[] = {
	{ PEN_TCG, PA_SUBTYPE_TCG_SWID }
};

static imc_agent_t *imc_swid;

/**
 * see section 3.8.1 of TCG TNC IF-IMC Specification 1.3
 */
TNC_Result TNC_IMC_Initialize(TNC_IMCID imc_id,
							  TNC_Version min_version,
							  TNC_Version max_version,
							  TNC_Version *actual_version)
{
	if (imc_swid)
	{
		DBG1(DBG_IMC, "IMC \"%s\" has already been initialized", imc_name);
		return TNC_RESULT_ALREADY_INITIALIZED;
	}
	imc_swid = imc_agent_create(imc_name, msg_types, countof(msg_types),
							  imc_id, actual_version);
	if (!imc_swid)
	{
		return TNC_RESULT_FATAL;
	}

	libpts_init();

	if (min_version > TNC_IFIMC_VERSION_1 || max_version < TNC_IFIMC_VERSION_1)
	{
		DBG1(DBG_IMC, "no common IF-IMC version");
		return TNC_RESULT_NO_COMMON_VERSION;
	}
	return TNC_RESULT_SUCCESS;
}

/**
 * see section 3.8.2 of TCG TNC IF-IMC Specification 1.3
 */
TNC_Result TNC_IMC_NotifyConnectionChange(TNC_IMCID imc_id,
										  TNC_ConnectionID connection_id,
										  TNC_ConnectionState new_state)
{
	imc_state_t *state;

	if (!imc_swid)
	{
		DBG1(DBG_IMC, "IMC \"%s\" has not been initialized", imc_name);
		return TNC_RESULT_NOT_INITIALIZED;
	}
	switch (new_state)
	{
		case TNC_CONNECTION_STATE_CREATE:
			state = imc_swid_state_create(connection_id);
			return imc_swid->create_state(imc_swid, state);
		case TNC_CONNECTION_STATE_HANDSHAKE:
			if (imc_swid->change_state(imc_swid, connection_id, new_state,
				&state) != TNC_RESULT_SUCCESS)
			{
				return TNC_RESULT_FATAL;
			}
			state->set_result(state, imc_id,
							  TNC_IMV_EVALUATION_RESULT_DONT_KNOW);
			return TNC_RESULT_SUCCESS;
		case TNC_CONNECTION_STATE_DELETE:
			return imc_swid->delete_state(imc_swid, connection_id);
		default:
			return imc_swid->change_state(imc_swid, connection_id,
										  new_state, NULL);
	}
}

/**
 * see section 3.8.3 of TCG TNC IF-IMC Specification 1.3
 */
TNC_Result TNC_IMC_BeginHandshake(TNC_IMCID imc_id,
								  TNC_ConnectionID connection_id)
{
	imc_state_t *state;

	if (!imc_swid)
	{
		DBG1(DBG_IMC, "IMC \"%s\" has not been initialized", imc_name);
		return TNC_RESULT_NOT_INITIALIZED;
	}
	if (!imc_swid->get_state(imc_swid, connection_id, &state))
	{
		return TNC_RESULT_FATAL;
	}

	return TNC_RESULT_SUCCESS;
}

static TNC_Result receive_message(imc_state_t *state, imc_msg_t *in_msg)
{
	imc_msg_t *out_msg;
	imc_swid_state_t *swid_state;
	enumerator_t *enumerator;
	pa_tnc_attr_t *attr;
	pen_type_t type;
	TNC_Result result;
	bool fatal_error = FALSE;

	/* parse received PA-TNC message and handle local and remote errors */
	result = in_msg->receive(in_msg, &fatal_error);
	if (result != TNC_RESULT_SUCCESS)
	{
		return result;
	}
	out_msg = imc_msg_create_as_reply(in_msg);
	swid_state = (imc_swid_state_t*)state;

	/* analyze PA-TNC attributes */
	enumerator = in_msg->create_attribute_enumerator(in_msg);
	while (enumerator->enumerate(enumerator, &attr))
	{
		tcg_swid_attr_req_t *attr_req;
		u_int8_t flags;
		u_int32_t request_id, eid_epoch;

		type = attr->get_type(attr);

		if (type.vendor_id != PEN_TCG || type.type != TCG_SWID_REQUEST)
		{
			continue;
		}

		attr_req = (tcg_swid_attr_req_t*)attr;
		flags = attr_req->get_flags(attr_req);
		request_id = attr_req->get_request_id(attr_req);
		eid_epoch = swid_state->get_eid_epoch(swid_state);

		if (flags & TCG_SWID_ATTR_REQ_FLAG_R)
		{
			swid_tag_id_t *tag_id;
			tcg_swid_attr_tag_id_inv_t *attr_cast;

			attr = tcg_swid_attr_tag_id_inv_create(request_id, eid_epoch, 1);
			attr_cast = (tcg_swid_attr_tag_id_inv_t*)attr;
			tag_id = swid_tag_id_create(
						chunk_from_str("regid.2004-03.org.strongswan"),
						chunk_from_str("strongSwan-5-1-1-dr1"),
						chunk_empty);
			attr_cast->add_tag_id(attr_cast, tag_id);
		}
		else
		{
			swid_tag_t *tag;
			tcg_swid_attr_tag_inv_t *attr_cast;
	
			attr = tcg_swid_attr_tag_inv_create(request_id, eid_epoch, 1);
			attr_cast = (tcg_swid_attr_tag_inv_t*)attr;
			tag = swid_tag_create(chunk_from_str(strongswan_tag), chunk_empty);
			attr_cast->add_tag(attr_cast, tag);
		}
		out_msg->add_attribute(out_msg, attr);
	}
	enumerator->destroy(enumerator);

	if (fatal_error)
	{
		result = TNC_RESULT_FATAL;
	}
	else
	{
		result = out_msg->send(out_msg, TRUE);
	}
	out_msg->destroy(out_msg);

	return result;
}

/**
 * see section 3.8.4 of TCG TNC IF-IMC Specification 1.3

 */
TNC_Result TNC_IMC_ReceiveMessage(TNC_IMCID imc_id,
								  TNC_ConnectionID connection_id,
								  TNC_BufferReference msg,
								  TNC_UInt32 msg_len,
								  TNC_MessageType msg_type)
{
	imc_state_t *state;
	imc_msg_t *in_msg;
	TNC_Result result;

	if (!imc_swid)
	{
		DBG1(DBG_IMC, "IMC \"%s\" has not been initialized", imc_name);
		return TNC_RESULT_NOT_INITIALIZED;
	}
	if (!imc_swid->get_state(imc_swid, connection_id, &state))
	{
		return TNC_RESULT_FATAL;
	}
	in_msg = imc_msg_create_from_data(imc_swid, state, connection_id, msg_type,
									  chunk_create(msg, msg_len));
	result = receive_message(state, in_msg);
	in_msg->destroy(in_msg);

	return result;
}

/**
 * see section 3.8.6 of TCG TNC IF-IMV Specification 1.3
 */
TNC_Result TNC_IMC_ReceiveMessageLong(TNC_IMCID imc_id,
									  TNC_ConnectionID connection_id,
									  TNC_UInt32 msg_flags,
									  TNC_BufferReference msg,
									  TNC_UInt32 msg_len,
									  TNC_VendorID msg_vid,
									  TNC_MessageSubtype msg_subtype,
									  TNC_UInt32 src_imv_id,
									  TNC_UInt32 dst_imc_id)
{
	imc_state_t *state;
	imc_msg_t *in_msg;
	TNC_Result result;

	if (!imc_swid)
	{
		DBG1(DBG_IMC, "IMC \"%s\" has not been initialized", imc_name);
		return TNC_RESULT_NOT_INITIALIZED;
	}
	if (!imc_swid->get_state(imc_swid, connection_id, &state))
	{
		return TNC_RESULT_FATAL;
	}
	in_msg = imc_msg_create_from_long_data(imc_swid, state, connection_id,
								src_imv_id, dst_imc_id,msg_vid, msg_subtype,
								chunk_create(msg, msg_len));
	result =receive_message(state, in_msg);
	in_msg->destroy(in_msg);

	return result;
}

/**
 * see section 3.8.7 of TCG TNC IF-IMC Specification 1.3
 */
TNC_Result TNC_IMC_BatchEnding(TNC_IMCID imc_id,
							   TNC_ConnectionID connection_id)
{
	if (!imc_swid)
	{
		DBG1(DBG_IMC, "IMC \"%s\" has not been initialized", imc_name);
		return TNC_RESULT_NOT_INITIALIZED;
	}
	return TNC_RESULT_SUCCESS;
}

/**
 * see section 3.8.8 of TCG TNC IF-IMC Specification 1.3
 */
TNC_Result TNC_IMC_Terminate(TNC_IMCID imc_id)
{
	if (!imc_swid)
	{
		DBG1(DBG_IMC, "IMC \"%s\" has not been initialized", imc_name);
		return TNC_RESULT_NOT_INITIALIZED;
	}

	libpts_deinit();

	imc_swid->destroy(imc_swid);
	imc_swid = NULL;

	return TNC_RESULT_SUCCESS;
}

/**
 * see section 4.2.8.1 of TCG TNC IF-IMC Specification 1.3
 */
TNC_Result TNC_IMC_ProvideBindFunction(TNC_IMCID imc_id,
									   TNC_TNCC_BindFunctionPointer bind_function)
{
	if (!imc_swid)
	{
		DBG1(DBG_IMC, "IMC \"%s\" has not been initialized", imc_name);
		return TNC_RESULT_NOT_INITIALIZED;
	}
	return imc_swid->bind_functions(imc_swid, bind_function);
}
