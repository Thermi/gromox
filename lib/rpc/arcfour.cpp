// SPDX-License-Identifier: GPL-3.0-or-later
/*
   Unix SMB/CIFS implementation.

   An implementation of the arcfour algorithm

   Copyright (C) Andrew Tridgell 1998

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <gromox/arcfour.hpp>
#include <cstdint>

/* initialise the arcfour sbox with key */
void arcfour_init(ARCFOUR_STATE *pstate, const DATA_BLOB *pkey) 
{
	uint8_t tc;
	uint8_t j = 0;
	
	for (size_t i = 0; i < sizeof(pstate->sbox); ++i)
		pstate->sbox[i] = (uint8_t)i;
	for (size_t i = 0; i < sizeof(pstate->sbox); ++i) {
		j += (pstate->sbox[i] + pkey->data[i%pkey->length]);
		
		tc = pstate->sbox[i];
		pstate->sbox[i] = pstate->sbox[j];
		pstate->sbox[j] = tc;
	}
	pstate->index_i = 0;
	pstate->index_j = 0;
}

void arcfour_destroy(ARCFOUR_STATE *pstate)
{
	/* do nothing */
}

/* crypt the data with arcfour */
void arcfour_crypt_sbox(ARCFOUR_STATE *pstate, uint8_t *pdata, int len) 
{
	int i;
	uint8_t t;
	uint8_t tc;
	
	for (i=0; i<len; i++) {
		
		pstate->index_i++;
		pstate->index_j += pstate->sbox[pstate->index_i];

		tc = pstate->sbox[pstate->index_i];
		pstate->sbox[pstate->index_i] = pstate->sbox[pstate->index_j];
		pstate->sbox[pstate->index_j] = tc;
		
		t = pstate->sbox[pstate->index_i] + pstate->sbox[pstate->index_j];
		pdata[i] = pdata[i] ^ pstate->sbox[t];
	}
}

/* arcfour encryption with a blob key */
void arcfour_crypt_blob(uint8_t *pdata, int len, const DATA_BLOB *pkey) 
{
	ARCFOUR_STATE state;
	
	arcfour_init(&state, pkey);
	arcfour_crypt_sbox(&state, pdata, len);
	arcfour_destroy(&state);
}

/*
  a variant that assumes a 16 byte key. This should be removed
  when the last user is gone
*/
void arcfour_crypt(uint8_t *pdata, const uint8_t keystr[16], int len)
{
	DATA_BLOB key;

	key.data = (uint8_t*)keystr;
	key.length = 16;
	
	arcfour_crypt_blob(pdata, len, &key);
	
}


