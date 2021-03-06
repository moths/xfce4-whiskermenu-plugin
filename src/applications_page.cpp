// Copyright (C) 2013 Graeme Gott <graeme@gottcode.org>
//
// This library is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this library.  If not, see <http://www.gnu.org/licenses/>.


#include "applications_page.hpp"

#include "launcher.hpp"
#include "launcher_model.hpp"
#include "launcher_view.hpp"
#include "menu.hpp"
#include "section_button.hpp"

#include <algorithm>
#include <map>

extern "C"
{
#include <libxfce4util/libxfce4util.h>
}

using namespace WhiskerMenu;

//-----------------------------------------------------------------------------

ApplicationsPage::Category::Category(GarconMenuDirectory* directory)
{
	// Fetch icon
	const gchar* icon = garcon_menu_directory_get_icon_name(directory);
	if (G_LIKELY(icon))
	{
		m_icon.assign(icon);
	}

	// Fetch text
	const gchar* text = garcon_menu_directory_get_name(directory);
	if (G_LIKELY(text))
	{
		m_text.assign(text);
	}
}

//-----------------------------------------------------------------------------

ApplicationsPage::ApplicationsPage(Menu* menu) :
	FilterPage(menu),
	m_garcon_menu(NULL),
	m_current_category(NULL),
	m_loaded(false)
{
	// Set desktop environment for applications
	const gchar* desktop = g_getenv("XDG_CURRENT_DESKTOP");
	if (G_LIKELY(!desktop))
	{
		desktop = "XFCE";
	}
	else if (*desktop == '\0')
	{
		desktop = NULL;
	}
	garcon_set_environment(desktop);
}

//-----------------------------------------------------------------------------

ApplicationsPage::~ApplicationsPage()
{
	clear_applications();
}

//-----------------------------------------------------------------------------

Launcher* ApplicationsPage::get_application(const std::string& desktop_id) const
{
	std::map<std::string, Launcher*>::const_iterator i = m_items.find(desktop_id);
	return (i != m_items.end()) ? i->second : NULL;
}

//-----------------------------------------------------------------------------

void ApplicationsPage::apply_filter(GtkToggleButton* togglebutton)
{
	// Find category matching button
	std::map<SectionButton*, Category*>::const_iterator i, end = m_category_buttons.end();
	for (i = m_category_buttons.begin(); i != end; ++i)
	{
		if (GTK_TOGGLE_BUTTON(i->first->get_button()) == togglebutton)
		{
			break;
		}
	}
	if (i == end)
	{
		return;
	}

	// Apply filter
	m_current_category = i->second;
	refilter();
	m_current_category = NULL;

	// Scroll to top
	GtkTreeIter iter;
	GtkTreePath* path = gtk_tree_path_new_first();
	if (gtk_tree_model_get_iter(get_view()->get_model(), &iter, path))
	{
		get_view()->scroll_to_path(path);
		get_view()->unselect_all();
	}
	gtk_tree_path_free(path);
}

//-----------------------------------------------------------------------------

bool ApplicationsPage::on_filter(GtkTreeModel* model, GtkTreeIter* iter)
{
	if (!m_current_category)
	{
		return true;
	}

	Launcher* launcher = NULL;
	gtk_tree_model_get(model, iter, LauncherModel::COLUMN_LAUNCHER, &launcher, -1);
	if (!launcher)
	{
		return false;
	}

	const std::vector<Launcher*>& category = m_categories[m_current_category];
	return std::find(category.begin(), category.end(), launcher) != category.end();
}

//-----------------------------------------------------------------------------

void ApplicationsPage::invalidate_applications()
{
	m_loaded = false;
}

//-----------------------------------------------------------------------------

void ApplicationsPage::load_applications()
{
	if (m_loaded)
	{
		return;
	}

	// Remove previous menu data
	clear_applications();

	// Populate map of menu data
	m_garcon_menu = garcon_menu_new_applications();
	g_object_ref(m_garcon_menu);
	if (garcon_menu_load(m_garcon_menu, NULL, NULL))
	{
		g_signal_connect_swapped(m_garcon_menu, "reload-required", G_CALLBACK(ApplicationsPage::invalidate_applications_slot), this);
		load_menu(m_garcon_menu);
	}

	// Create sorted list of menu items
	std::map<std::string, Launcher*> sorted_items;
	for (std::map<std::string, Launcher*>::const_iterator i = m_items.begin(), end = m_items.end(); i != end; ++i)
	{
		gchar* collation_key = g_utf8_collate_key(i->second->get_text(), -1);
		sorted_items[collation_key] = i->second;
		g_free(collation_key);
	}

	// Add all items to model
	LauncherModel model;
	for (std::map<std::string, Launcher*>::const_iterator i = sorted_items.begin(), end = sorted_items.end(); i != end; ++i)
	{
		model.append_item(i->second);
	}

	// Create filter model and pass ownership to view, do not delete!
	set_model(model.get_model());

	// Update filters
	load_categories();

	// Update menu items of other panels
	get_menu()->set_items();

	m_loaded = true;
}

//-----------------------------------------------------------------------------

void ApplicationsPage::clear_applications()
{
	// Free categories
	for (std::map<SectionButton*, Category*>::iterator i = m_category_buttons.begin(), end = m_category_buttons.end(); i != end; ++i)
	{
		delete i->first;
	}
	m_category_buttons.clear();

	for (std::map<Category*, std::vector<Launcher*> >::iterator i = m_categories.begin(), end = m_categories.end(); i != end; ++i)
	{
		delete i->first;
	}
	m_categories.clear();

	// Free menu items
	get_menu()->unset_items();
	unset_model();

	for (std::map<std::string, Launcher*>::iterator i = m_items.begin(), end = m_items.end(); i != end; ++i)
	{
		delete i->second;
	}
	m_items.clear();

	// Unreference menu
	if (m_garcon_menu)
	{
		g_object_unref(m_garcon_menu);
		m_garcon_menu = NULL;
	}

	// Clear menu item cache
	GarconMenuItemCache* cache = garcon_menu_item_cache_get_default();
	garcon_menu_item_cache_invalidate(cache);
	g_object_unref(cache);
}

//-----------------------------------------------------------------------------

void ApplicationsPage::load_menu(GarconMenu* menu)
{
	GarconMenuDirectory* directory = garcon_menu_get_directory(menu);

	// Skip hidden categories
	if (directory && !garcon_menu_directory_get_visible(directory))
	{
		g_object_unref(directory);
		return;
	}

	// Only track single level of categories
	bool first_level = directory && (garcon_menu_get_parent(menu) == m_garcon_menu);
	if (first_level)
	{
		m_current_category = new Category(directory);
	}
	if (directory)
	{
		g_object_unref(directory);
	}

	// Add submenus
	GList* menus = garcon_menu_get_menus(menu);
	for (GList* li = menus; li != NULL; li = li->next)
	{
		load_menu(GARCON_MENU(li->data));
	}
	g_list_free(menus);

	// Add items
	GarconMenuItemPool* pool = garcon_menu_get_item_pool(menu);
	if (G_LIKELY(pool))
	{
		garcon_menu_item_pool_foreach(pool, (GHFunc)&ApplicationsPage::load_menu_item, this);
	}

	// Only track single level of categories
	if (first_level)
	{
		// Free unused categories
		std::map<Category*, std::vector<Launcher*> >::iterator i = m_categories.find(m_current_category);
		if (i == m_categories.end())
		{
			delete m_current_category;
		}
		// Do not track empty categories
		else if (i->second.empty())
		{
			m_categories.erase(i);
			delete m_current_category;
		}
		m_current_category = NULL;
	}

	// Listen for menu changes
	g_signal_connect_swapped(menu, "directory-changed", G_CALLBACK(ApplicationsPage::invalidate_applications_slot), this);
}

//-----------------------------------------------------------------------------

void ApplicationsPage::load_menu_item(const gchar* desktop_id, GarconMenuItem* menu_item, ApplicationsPage* page)
{
	// Skip hidden items
	if (!garcon_menu_element_get_visible(GARCON_MENU_ELEMENT(menu_item)))
	{
		return;
	}

	// Add to map
	std::string key(desktop_id);
	std::map<std::string, Launcher*>::iterator iter = page->m_items.find(key);
	if (iter == page->m_items.end())
	{
		iter = page->m_items.insert(std::make_pair(key, new Launcher(menu_item))).first;
	}

	// Add menu item to current category
	if (page->m_current_category)
	{
		page->m_categories[page->m_current_category].push_back(iter->second);
	}

	// Listen for menu changes
	g_signal_connect_swapped(menu_item, "changed", G_CALLBACK(ApplicationsPage::invalidate_applications_slot), page);
}

//-----------------------------------------------------------------------------

void ApplicationsPage::load_categories()
{
	std::vector<SectionButton*> category_buttons;

	// Add button for all applications
	SectionButton* all_button = new SectionButton("applications-other", _("All"));
	g_signal_connect(all_button->get_button(), "toggled", G_CALLBACK(ApplicationsPage::apply_filter_slot), this);
	m_category_buttons[all_button] = NULL;
	category_buttons.push_back(all_button);

	// Create sorted list of categories
	std::map<std::string, Category*> sorted_categories;
	for (std::map<Category*, std::vector<Launcher*> >::const_iterator i = m_categories.begin(), end = m_categories.end(); i != end; ++i)
	{
		gchar* collation_key = g_utf8_collate_key(i->first->get_text(), -1);
		sorted_categories[collation_key] = i->first;
		g_free(collation_key);
	}

	// Add buttons for sorted categories
	for (std::map<std::string, Category*>::const_iterator i = sorted_categories.begin(), end = sorted_categories.end(); i != end; ++i)
	{
		SectionButton* category_button = new SectionButton(i->second->get_icon(), i->second->get_text());
		g_signal_connect(category_button->get_button(), "toggled", G_CALLBACK(ApplicationsPage::apply_filter_slot), this);
		m_category_buttons[category_button] = i->second;
		category_buttons.push_back(category_button);
	}

	// Add category buttons to window
	get_menu()->set_categories(category_buttons);
}

//-----------------------------------------------------------------------------
