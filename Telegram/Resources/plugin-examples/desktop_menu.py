"""Manually adapted menu example: not an unmodified Android plugin."""
from base_plugin import BasePlugin, MenuItemData, MenuItemType
from ui.bulletin import BulletinHelper

__id__ = 'desktop_menu'
__name__ = 'Desktop message info'

class DesktopMenuPlugin(BasePlugin):
    def on_plugin_load(self):
        self.add_menu_item(MenuItemData(
            menu_type=MenuItemType.MESSAGE_CONTEXT_MENU,
            item_id='inspect',
            text='Показать ID сообщения',
            on_click=self.inspect,
        ))

    def inspect(self, context):
        # Desktop DTO supports these fields, not Java reflection/native methods.
        msg = context['message']
        BulletinHelper.show_info(f'ID: {msg.getId()} | {msg.messageText}')

    def on_plugin_unload(self):
        self.logger.info('DesktopMenuPlugin cleanup')
