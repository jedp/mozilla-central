/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * The contents of this file are subject to the Netscape Public License
 * Version 1.0 (the "NPL"); you may not use this file except in
 * compliance with the NPL.  You may obtain a copy of the NPL at
 * http://www.mozilla.org/NPL/
 *
 * Software distributed under the NPL is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the NPL
 * for the specific language governing rights and limitations under the
 * NPL.
 *
 * The Initial Developer of this code under the NPL is Netscape
 * Communications Corporation.  Portions created by Netscape are
 * Copyright (C) 1998 Netscape Communications Corporation.  All Rights
 * Reserved.
 */
/* 
   AddrBookView.h -- view of user's mailfilters.
   Created: Chris Toshok <toshok@netscape.com>, 29-Aug-96.
   Revised: Tao Cheng <tao@netscape.com>, 01-nov-96
 */

#include "AddrBookView.h"
#include "AddrBookFrame.h"

#include "Xfe/Xfe.h"

#include "xpassert.h"
#include "felocale.h" /* Bug Fixing by bstell on r.1.77.6.3 */

extern char *fe_conference_path;

#if defined(DEBUG_tao_)
#define TRACE(str)   printf("%s,%s", __FILE__, __LINE__)
#else
#define TRACE(str)   
#endif

extern "C" {
#include "xfe.h"
#include "outline.h"
#include "addrbk.h"

XP_List* FE_GetDirServers();
ABook*   fe_GetABook(MWContext *context);
};
extern "C" void 
fe_showConference(Widget w, char *email, short use, char *coolAddr);
extern "C" void 
fe_showABCardPropertyDlg(Widget parent, MWContext *context, ABID entry, 
						 XP_Bool newuser);

extern "C" void fe_showConference(Widget w, char *email, 
								  short use, char *coolAddr);

#include "xpgetstr.h"

extern int XFE_AB_HEADER_NAME;
extern int XFE_AB_HEADER_CERTIFICATE;
extern int XFE_AB_HEADER_EMAIL;
extern int XFE_AB_HEADER_NICKNAME;
extern int XFE_AB_HEADER_EMAIL;
extern int XFE_AB_HEADER_COMPANY;
extern int XFE_AB_HEADER_PHONE;
extern int XFE_AB_HEADER_LOCALITY;

extern int MK_MSG_CALL_NEEDS_EMAILADDRESS;
extern int XFE_PLACE_CONFERENCE_CALL_TO;
extern int XFE_SEND_MSG_TO;

#define ADDRESS_OUTLINER_GEOMETRY_PREF "addressbook.outliner_geometry"

// constructor
XFE_AddrBookView::XFE_AddrBookView(XFE_Component *toplevel_component, 
								   Widget         parent, 
								   XFE_View      *parent_view,
								   MWContext     *context,
								   XP_List       *directories):
	XFE_ABListSearchView(toplevel_component, 
						 parent,
						 parent_view, 
						 context,
						 directories){

  /* For outliner
   */
  int num_columns = OUTLINER_COLUMN_LAST;
  static int column_widths[] = {3, 25, 25, 8, 8, 10, 12};
  m_outliner = new XFE_Outliner("addressBookList",
								this,
								toplevel_component,
								parent,
								False, // constantSize
								True,  // hasHeadings
								num_columns,
								num_columns-2,// num_visible 
								column_widths,
								ADDRESS_OUTLINER_GEOMETRY_PREF);

  m_outliner->setMultiSelectAllowed(True);
  m_outliner->setHideColumnsAllowed(True);

  /* BEGIN_3P: XmLGrid
   */
  XtVaSetValues(m_outliner->getBaseWidget(),
				XmNselectionPolicy, XmSELECT_MULTIPLE_ROW,
                XtVaTypedArg, XmNblankBackground, XmRString, "white", 6,
				XmNvisibleRows, 15,
				NULL);
  XtVaSetValues(m_outliner->getBaseWidget(),
				XmNcellDefaults, True,
                XtVaTypedArg, XmNcellBackground, XmRString, "white", 6,
				NULL);
  /* END_3P: XmLGrid
   */
  // layout
  layout();

  setBaseWidget(m_outliner->getBaseWidget());

  /* For property
   */
  CONTEXT_DATA(m_contextData)->abdata->outline = m_outliner->getBaseWidget();
}

XFE_AddrBookView::~XFE_AddrBookView()
{
}

//
// tooltips and doc string
char *XFE_AddrBookView::getDocString(CommandType cmd)
{
	uint32 count = 0;
	const int *indices = 0;
	m_outliner->getSelection(&indices, (int *) &count);

	if (count > 0) {
		char *names = 0;
		char a_line[512];
		a_line[0] = '\0';
		int len = 0;
		for (int i=0; i < count && len < 512; i++) {
			/* Take the first one 
			 */
#if defined(USE_ABCOM)
			AB_AttributeValue *value = NULL;
			int error = 
				AB_GetEntryAttributeForPane(m_pane,
											(MSG_ViewIndex) indices[i],
											AB_attribFullName,
											&value);
			XP_ASSERT(value && value->attrib == AB_attribFullName);

			XP_SAFE_SPRINTF(a_line, sizeof(a_line),
							"%s",
							value->u.string?value->u.string:"");
			AB_FreeEntryAttributeValue(value);
#else
			ABID entry;		
			entry = AB_GetEntryIDAt((AddressPane *) m_abPane, 
									(uint32) indices[i]);
			AB_GetFullName(m_dir, m_AddrBook, entry, a_line);
#endif /* USE_ABCOM */
			if (a_line) {
				len += XP_STRLEN(a_line);
				if (i)
					len += 2;

				names = names?
					((char *) XP_REALLOC(names, (len+1)*sizeof(char))):
					((char *) XP_CALLOC(1+len, sizeof(char)));

				if (i)
					names = XP_STRCAT(names, ", ");
				names = XP_STRCAT(names, a_line);			
			}/* if */
		}/* for i */

		char *cstr = 0;
		if (cmd == xfeCmdComposeMessage ||
			cmd == xfeCmdComposeMessageHTML ||
			cmd == xfeCmdComposeMessagePlain) {
			cstr = XP_STRDUP(XP_GetString(XFE_SEND_MSG_TO));	
		} else if (cmd == xfeCmdABCall) {
			cstr = XP_STRDUP(XP_GetString(XFE_PLACE_CONFERENCE_CALL_TO));
		}
		len += XP_STRLEN(cstr);
		cstr = (char *) XP_REALLOC(cstr, (1+len)*sizeof(char));
		cstr = (char *) XP_STRCAT(cstr, names);
		return cstr;
	}/* if */
	return NULL;
}

Boolean XFE_AddrBookView::isCommandSelected(CommandType cmd, 
									void */* calldata */,
									XFE_CommandInfo* /* i */)
{

	/* 	
	 *	XP_Bool AB_GetPaneSortedAscending(ABPane * pABookPane);
	 *	ABID AB_GetPaneSortedBy(ABPane * pABookPane);
	 */
	ABID sortType = 0;

	if (cmd == xfeCmdABByType)
		sortType = ABTypeEntry;
	else if (cmd == xfeCmdABByName)
		sortType = ABFullName;
	else if (cmd == xfeCmdABByEmailAddress)
		sortType = ABEmailAddress;
	else if (cmd == xfeCmdABByCompany)
		sortType = ABCompany;
	else if (cmd == xfeCmdABByLocality)
		sortType = ABLocality;
	else if (cmd == xfeCmdABByNickName)
		sortType = ABNickname;

	if (sortType > 0) {
		if (sortType == getSortType())
			return True;
		return False;
	}/* if */
	else if ((cmd == xfeCmdSortAscending && isAscending())||
			 (cmd == xfeCmdSortDescending && !isAscending()))
		return True;
	return False;
}

void XFE_AddrBookView::doubleClickBody(const OutlineButtonFuncData *data)
{
#if defined(USE_ABCOM)
	editProperty();
#else
  int row = data->row;
  ABID type, entry;

  entry = AB_GetEntryIDAt((AddressPane *) m_abPane, (uint32) row);
    
  if (entry == MSG_VIEWINDEXNONE) 
    return;
    
  AB_GetType(m_dir, m_AddrBook, entry, &type);
    
  if (type == ABTypePerson)
      /* Send to this user
       */
    popupUserPropertyWindow(entry, False, False);
  else
    /* Send to this mail list
     */
    popupListPropertyWindow(entry, False, False);
#endif /* USE_ABCOM */
}

void XFE_AddrBookView::abCall()
{
  int count = 0;
  const int *indices = 0;
  m_outliner->getSelection(&indices, &count);
  if (count > 0 && indices) {
#if defined(USE_ABCOM)
#else
	  /* Take the first one 
	   */
	  ABID type;
	  ABID entry;
	  
	  entry = AB_GetEntryIDAt((AddressPane *) m_abPane, (uint32) indices[0]);
    
	  if (entry == MSG_VIEWINDEXNONE) 
		  return;

	  AB_GetType(m_dir, m_AddrBook, entry, &type);
    
	  /* Select
	   */
	  if (type != ABTypePerson)
		  return;
		  
	  char a_line[AB_MAX_STRLEN];
	  a_line[0] = '\0';
	  
	  char cmdstr[AB_MAX_STRLEN];
	  cmdstr[0] = '\0';
	  char *email= NULL;

	  if ((AB_GetEmailAddress(m_dir, m_AddrBook, entry, a_line) != MSG_VIEWINDEXNONE)&&
		  XP_STRLEN(a_line))
		  email = XP_STRDUP(a_line);
	  else {
		  /* Can not call w/o email
		   */
		  // Prompt the user to enter the email address
		  char tmp[128];
		  XP_SAFE_SPRINTF(tmp, sizeof(tmp),
						  "%s",
						  XP_GetString(MK_MSG_CALL_NEEDS_EMAILADDRESS));
		  fe_Message(m_contextData, tmp);
		  return;
	  }/* else */

	  char *coolAddr = NULL;
	  if (AB_GetCoolAddress(m_dir, m_AddrBook, entry, a_line) != MSG_VIEWINDEXNONE)
		  coolAddr = XP_STRDUP(a_line);

	  short use;
	  if (AB_GetUseServer(m_dir, m_AddrBook, entry, &use) == MSG_VIEWINDEXNONE)
		  /* Can't determine which mode; use default
		   */
		  use = kDefaultDLS;

	  if (email && XP_STRLEN(email))
		  fe_showConference(getBaseWidget(), email, use, coolAddr);
	  else {
		  /* prompt user to enter email address for this person
		   */
			  char tmp[128];
			  XP_SAFE_SPRINTF(tmp, sizeof(tmp),
							  "%s",
							  XP_GetString(MK_MSG_CALL_NEEDS_EMAILADDRESS));
			  fe_Message(m_contextData, tmp);

	  }/* else */
	  if (email)
		  XP_FREE((char *) email);
	  if (coolAddr)
		  XP_FREE((char *) coolAddr);
#endif /* USE_ABCOM */
  }/* if */
}

void XFE_AddrBookView::abVCard()
{
	// Need to check if the user has created a card 
	XFE_GlobalPrefs    *prefs = &fe_globalPrefs;

	PersonEntry  person;
	ABook       *addr_book = fe_GetABook(0);
	DIR_Server  *dir;
	ABID         entry_id;

	person.Initialize();
	person.pGivenName = ((prefs->real_name) && 
						 (XP_STRLEN(prefs->real_name) > 0)) ?
		                 XP_STRDUP(prefs->real_name) : 0;
	person.pEmailAddress = ((prefs->email_address) && 
							(XP_STRLEN(prefs->email_address) > 0)) ?
		                    XP_STRDUP(prefs->email_address) : 0;
  
	DIR_GetPersonalAddressBook(m_directories, &dir);

	if (dir) {
		AB_GetEntryIDForPerson(dir, addr_book, &entry_id, &person);
		fe_showABCardPropertyDlg(getToplevel()->getBaseWidget(), 
								 m_contextData,
								 entry_id,
								 (MSG_MESSAGEIDNONE != entry_id)?FALSE:TRUE);
	}
	person.CleanUp();

}

//
extern "C" void fe_showConference(Widget w, char *email, 
								  short use, char *coolAddr)
{
	// Assume that conference is located at the same directory as netscape
	XP_ASSERT(w);

	char execu[1024];
	XP_STRCPY(execu, fe_conference_path);

#if defined(IRIX)
	pid_t mypid = fork();
#else
	pid_t mypid = vfork();
#endif
	if (mypid == 0) {

		close (ConnectionNumber(XtDisplay(w)));

		/* sports
		 * int execlp (const char *file, const char *arg0, ..., 
		 * int execvp (const char *file, char *const *argv);
		 * const char *argn, (char *)0);
		 */
		if (email && 
			XP_STRLEN(email)) {
			if (use == kDefaultDLS ||
				!coolAddr || 
				XP_STRLEN(coolAddr) == 0) {
				execlp(execu, execu, 
					   "-invite", email, 0);
			}/* if */
			else if (use == kSpecificDLS && 
					 coolAddr && 
					 XP_STRLEN(coolAddr)) {
				execlp(execu, execu, 
					   "-invite", email, 
					   "-server", coolAddr, 0);
			}/* else */
			else if (use == kHostOrIPAddress && 
					 coolAddr && 
					 XP_STRLEN(coolAddr)) {
				execlp(execu, execu, 
					   "-invite", email, 
					   "-direct", coolAddr, 0);
			}/* else */
		}/* if */
		else
			/* insufficient info to run
			 */
			execlp(execu, execu, 0);

		_exit(0);
	}/* if */
}/* fe_showConference() */
