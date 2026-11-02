#!/usr/bin/env bash
#
# Copyright (C) 2026 Bongani Mbigi
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 2 or (at your option)
# version 3 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
# Keeps the Azure CLI signed in for Artifact Signing while a build runs.
#
# azure/login signs in with a GitHub OIDC token that expires after a few minutes, and the CLI cannot
# renew a federated login by itself, so SignTool's dlib fails with AADSTS700024 once the build takes
# longer than that. This signs in again with a fresh token every few minutes and fetches the code
# signing token ahead of SignTool. Run it in the background; it needs id-token: write and
# AZURE_CLIENT_ID and AZURE_TENANT_ID in its environment.

while true; do
    token=$(curl -fsS -H "Authorization: Bearer $ACTIONS_ID_TOKEN_REQUEST_TOKEN" \
                "$ACTIONS_ID_TOKEN_REQUEST_URL&audience=api://AzureADTokenExchange" \
            | python -c 'import json, sys; print(json.load(sys.stdin)["value"])')
    if [ -n "$token" ] \
        && az login --service-principal --username "$AZURE_CLIENT_ID" --tenant "$AZURE_TENANT_ID" \
                    --federated-token "$token" --allow-no-subscriptions --output none \
        && az account get-access-token --resource https://codesigning.azure.net --output none; then
        echo "Azure login refreshed for Artifact Signing"
    else
        echo "::warning::Could not refresh the Azure login for Artifact Signing"
    fi
    sleep 240
done
